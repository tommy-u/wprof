//! An ordered pool for opaque C jobs. A full ring retires its oldest job before
//! reusing that worker, so each worker owns at most one pending job and sending
//! never waits for space. Workers may finish out of order; completion callbacks
//! run in submission order on the caller. Finish closes every input channel,
//! drains pending results, then joins all workers before releasing the pool.

use core::ffi::c_void;
use std::fmt::Display;
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::thread::{self, JoinHandle};

type Callback = unsafe extern "C" fn(*mut c_void, *mut c_void);

struct Job(*mut c_void);
// SAFETY: C transfers exclusive job access to one worker until work returns,
// then to the caller's done callback, which may free it.
unsafe impl Send for Job {}

#[derive(Clone, Copy)]
struct Context(*mut c_void);
// SAFETY: C keeps ctx alive through finish and ensures any shared access is safe.
// Work may use shared ctx only with synchronization; done runs on the caller.
unsafe impl Send for Context {}

impl Context {
    unsafe fn call(self, callback: Callback, job: &Job) {
        callback(self.0, job.0);
    }
}

struct Worker {
    jobs: Option<SyncSender<Job>>,
    results: Receiver<Job>,
    thread: JoinHandle<()>,
}

pub struct WPool {
    workers: Vec<Worker>,
    head: usize,
    pending: usize,
    done: Callback,
    ctx: Context,
}

fn failed(operation: &str, error: impl Display) -> ! {
    eprintln!("Failed to {operation} through Rust worker pool: {error}");
    std::process::exit(1);
}

impl WPool {
    fn drain(&mut self) {
        let job = self.workers[self.head]
            .results
            .recv()
            .unwrap_or_else(|e| failed("receive completed job", e));
        unsafe { self.ctx.call(self.done, &job) };
        self.head = (self.head + 1) % self.workers.len();
        self.pending -= 1;
    }

    fn finish(mut self) {
        for worker in &mut self.workers {
            worker.jobs.take();
        }
        while self.pending > 0 {
            self.drain();
        }
        for worker in self.workers {
            worker
                .thread
                .join()
                .unwrap_or_else(|_| failed("join worker", "thread panicked"));
        }
    }
}

fn new_with_cpus(
    available: usize,
    max: usize,
    work: Callback,
    done: Callback,
    ctx: *mut c_void,
) -> *mut WPool {
    let count = available.min(max);
    if count < 2 {
        return core::ptr::null_mut();
    }
    let ctx = Context(ctx);
    let mut workers = Vec::with_capacity(count);
    for i in 0..count {
        let (jobs, input) = sync_channel::<Job>(1);
        let (output, results) = sync_channel(1);
        let worker = thread::Builder::new()
            .name(format!("wprof_pool{i}"))
            .spawn(move || {
                while let Ok(job) = input.recv() {
                    unsafe { ctx.call(work, &job) };
                    output
                        .send(job)
                        .unwrap_or_else(|e| failed("return completed job", e));
                }
            })
            .unwrap_or_else(|e| failed("start worker", e));
        workers.push(Worker {
            jobs: Some(jobs),
            results,
            thread: worker,
        });
    }
    Box::into_raw(Box::new(WPool {
        workers,
        head: 0,
        pending: 0,
        done,
        ctx,
    }))
}

#[no_mangle]
pub extern "C" fn wpool_new(
    max_workers: usize,
    work: Callback,
    done: Callback,
    ctx: *mut c_void,
) -> *mut WPool {
    let available =
        thread::available_parallelism().unwrap_or_else(|e| failed("query available CPUs", e));
    new_with_cpus(available.get(), max_workers, work, done, ctx)
}

#[no_mangle]
pub unsafe extern "C" fn wpool_submit(pool: *mut WPool, job: *mut c_void) {
    let pool = &mut *pool;
    if pool.pending == pool.workers.len() {
        pool.drain();
    }
    let slot = (pool.head + pool.pending) % pool.workers.len();
    pool.workers[slot]
        .jobs
        .as_ref()
        .unwrap()
        .send(Job(job))
        .unwrap_or_else(|e| failed("submit job", e));
    pool.pending += 1;
}

#[no_mangle]
pub unsafe extern "C" fn wpool_finish(pool: *mut WPool) {
    if !pool.is_null() {
        (*Box::from_raw(pool)).finish();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::c_void;
    use std::sync::{Arc, Condvar, Mutex};
    use std::thread::{self, ThreadId};

    #[derive(Default)]
    struct Gate {
        completed: Mutex<Vec<usize>>,
        changed: Condvar,
    }

    struct TestJob {
        id: usize,
        result: Option<u64>,
        worker: Option<ThreadId>,
        gate: Option<Arc<Gate>>,
    }

    struct Completed {
        id: usize,
        result: Option<u64>,
        worker: Option<ThreadId>,
        done_caller: ThreadId,
    }

    #[derive(Default)]
    struct Output {
        jobs: Vec<Completed>,
    }

    unsafe extern "C" fn work(_ctx: *mut c_void, job: *mut c_void) {
        let job = &mut *job.cast::<TestJob>();
        job.result = Some(job.id as u64 * 3 + 1);
        job.worker = Some(thread::current().id());
        if let Some(gate) = &job.gate {
            let mut completed = gate.completed.lock().unwrap();
            if job.id == 0 {
                completed = gate
                    .changed
                    .wait_while(completed, |ids| !ids.contains(&1))
                    .unwrap();
            }
            completed.push(job.id);
            gate.changed.notify_all();
        }
    }

    unsafe extern "C" fn done(ctx: *mut c_void, job: *mut c_void) {
        let out = &mut *ctx.cast::<Output>();
        let job = Box::from_raw(job.cast::<TestJob>());
        out.jobs.push(Completed {
            id: job.id,
            result: job.result,
            worker: job.worker,
            done_caller: thread::current().id(),
        });
    }

    fn job(id: usize, gate: Option<Arc<Gate>>) -> *mut c_void {
        Box::into_raw(Box::new(TestJob {
            id,
            result: None,
            worker: None,
            gate,
        }))
        .cast()
    }

    fn pool(available: usize, max: usize, out: &mut Output) -> *mut WPool {
        let pool = new_with_cpus(available, max, work, done, (out as *mut Output).cast());
        assert!(!pool.is_null());
        pool
    }

    fn ids(out: &Output) -> Vec<usize> {
        out.jobs.iter().map(|job| job.id).collect()
    }

    #[test]
    fn done_preserves_order_when_work_completes_out_of_order() {
        let gate = Arc::new(Gate::default());
        let mut out = Output::default();
        let pool = pool(2, 4, &mut out);
        unsafe {
            wpool_submit(pool, job(0, Some(gate.clone())));
            wpool_submit(pool, job(1, Some(gate.clone())));
            wpool_finish(pool);
        }
        assert_eq!(*gate.completed.lock().unwrap(), [1, 0]);
        assert_eq!(ids(&out), [0, 1]);
        assert_eq!(out.jobs[0].result, Some(1));
        assert_eq!(out.jobs[1].result, Some(4));
    }

    #[test]
    fn submit_retires_oldest_job_when_the_ring_is_full() {
        let mut out = Output::default();
        let pool = pool(4, 2, &mut out);
        unsafe {
            wpool_submit(pool, job(0, None));
            wpool_submit(pool, job(1, None));
            assert!(out.jobs.is_empty());
            wpool_submit(pool, job(2, None));
            assert_eq!(ids(&out), [0]);
            wpool_submit(pool, job(3, None));
            assert_eq!(ids(&out), [0, 1]);
            wpool_finish(pool);
        }
        assert_eq!(ids(&out), [0, 1, 2, 3]);
        assert_eq!(
            out.jobs.iter().map(|job| job.result).collect::<Vec<_>>(),
            [Some(1), Some(4), Some(7), Some(10)]
        );
    }

    #[test]
    fn finish_drains_a_partial_ring() {
        let mut out = Output::default();
        let pool = pool(4, 4, &mut out);
        unsafe {
            wpool_submit(pool, job(0, None));
            wpool_submit(pool, job(1, None));
            wpool_submit(pool, job(2, None));
            assert!(out.jobs.is_empty());
            wpool_finish(pool);
        }
        assert_eq!(ids(&out), [0, 1, 2]);
        assert_eq!(
            out.jobs.iter().map(|job| job.result).collect::<Vec<_>>(),
            [Some(1), Some(4), Some(7)]
        );
    }

    #[test]
    fn work_runs_on_workers_and_done_runs_on_the_caller() {
        let caller = thread::current().id();
        let mut out = Output::default();
        let pool = pool(4, 3, &mut out);
        unsafe {
            for id in 0..7 {
                wpool_submit(pool, job(id, None));
            }
            wpool_finish(pool);
        }
        assert_eq!(out.jobs.len(), 7);
        for job in &out.jobs {
            assert!(job.worker.is_some());
            assert_ne!(job.worker, Some(caller));
            assert_eq!(job.done_caller, caller);
        }
    }

    #[test]
    fn no_jobs_finish_produces_no_callbacks() {
        let mut out = Output::default();
        let pool = pool(4, 4, &mut out);
        unsafe {
            wpool_finish(pool);
            wpool_finish(core::ptr::null_mut());
        }
        assert!(out.jobs.is_empty());
    }

    #[test]
    fn fewer_than_two_selected_cpus_returns_serial_fallback() {
        let mut out = Output::default();
        for (available, max) in [(1, 4), (8, 1), (8, 0)] {
            let pool = new_with_cpus(available, max, work, done, (&mut out as *mut Output).cast());
            assert!(pool.is_null());
        }
        assert!(out.jobs.is_empty());
    }

    #[test]
    fn cpu_availability_and_requested_cap_limit_outstanding_jobs() {
        for (available, max, selected) in [(2, 4, 2), (8, 2, 2), (8, 4, 4)] {
            let mut out = Output::default();
            let pool = pool(available, max, &mut out);
            unsafe {
                for id in 0..selected {
                    wpool_submit(pool, job(id, None));
                }
                assert!(out.jobs.is_empty());
                wpool_submit(pool, job(selected, None));
                assert_eq!(ids(&out), [0]);
                wpool_finish(pool);
            }
            assert_eq!(ids(&out), (0..=selected).collect::<Vec<_>>());
        }
    }
}
