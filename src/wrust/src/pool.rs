//! Ordered pools for opaque C jobs. The legacy pool retires its oldest job on
//! the caller before reusing a worker, so each worker owns at most one pending
//! job and sends never wait for space. Workers may finish out of order; legacy
//! done callbacks run in submission order on the caller. The windowed pool
//! shares a FIFO input queue and limits jobs and estimated bytes at submission.
//! Its dedicated writer runs done in submission order and releases admission
//! credits only after done returns; workers never wait for output budget.
//! Finish closes input, drains results, and joins all threads before ctx expires.

use core::ffi::c_void;
use std::collections::{BTreeMap, VecDeque};
use std::fmt::Display;
use std::sync::mpsc::{channel, sync_channel, Receiver, SyncSender};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

type Callback = unsafe extern "C" fn(*mut c_void, *mut c_void);

struct Job(*mut c_void);
// SAFETY: C transfers exclusive job access to one worker until work returns,
// then to the ordered done callback, which may free it.
unsafe impl Send for Job {}

#[derive(Clone, Copy)]
struct Context(*mut c_void);
// SAFETY: C keeps ctx alive through finish and ensures any shared access is safe.
// Work and windowed done may use shared ctx only with synchronization.
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

struct RingPool {
    workers: Vec<Worker>,
    head: usize,
    pending: usize,
    done: Callback,
    ctx: Context,
}

pub struct WPool {
    mode: PoolMode,
}

enum PoolMode {
    Ring(RingPool),
    Windowed(WindowedPool),
}

struct WindowJob {
    job: Job,
    sequence: usize,
    admission_bytes: usize,
}

struct WindowState {
    jobs: VecDeque<WindowJob>,
    closed: bool,
    next_sequence: usize,
    in_flight: usize,
    admission_bytes: u128,
}

struct WindowShared {
    state: Mutex<WindowState>,
    input_ready: Condvar,
    admission_ready: Condvar,
    job_limit: usize,
    byte_limit: u128,
}

struct WindowedPool {
    shared: Arc<WindowShared>,
    workers: Vec<JoinHandle<()>>,
    writer: JoinHandle<()>,
}

fn failed(operation: &str, error: impl Display) -> ! {
    eprintln!("Failed to {operation} through Rust worker pool: {error}");
    std::process::exit(1);
}

impl RingPool {
    fn drain(&mut self) {
        let job = self.workers[self.head]
            .results
            .recv()
            .unwrap_or_else(|e| failed("receive completed job", e));
        unsafe { self.ctx.call(self.done, &job) };
        self.head = (self.head + 1) % self.workers.len();
        self.pending -= 1;
    }

    fn submit(&mut self, job: Job) {
        if self.pending == self.workers.len() {
            self.drain();
        }
        let slot = (self.head + self.pending) % self.workers.len();
        self.workers[slot]
            .jobs
            .as_ref()
            .unwrap()
            .send(job)
            .unwrap_or_else(|e| failed("submit job", e));
        self.pending += 1;
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

impl WindowedPool {
    fn submit(&self, job: Job, admission_bytes: usize) {
        let mut state = self.shared.state.lock().unwrap();
        while state.in_flight >= self.shared.job_limit
            || (state.in_flight != 0
                && state.admission_bytes + admission_bytes as u128 > self.shared.byte_limit)
        {
            state = self.shared.admission_ready.wait(state).unwrap();
        }
        let sequence = state.next_sequence;
        state.next_sequence += 1;
        state.in_flight += 1;
        state.admission_bytes += admission_bytes as u128;
        state.jobs.push_back(WindowJob {
            job,
            sequence,
            admission_bytes,
        });
        self.shared.input_ready.notify_one();
    }

    fn finish(self) {
        self.shared.state.lock().unwrap().closed = true;
        self.shared.input_ready.notify_all();
        for worker in self.workers {
            worker
                .join()
                .unwrap_or_else(|_| failed("join worker", "thread panicked"));
        }
        self.writer
            .join()
            .unwrap_or_else(|_| failed("join writer", "thread panicked"));
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
        mode: PoolMode::Ring(RingPool {
            workers,
            head: 0,
            pending: 0,
            done,
            ctx,
        }),
    }))
}

fn new_windowed_with_cpus(
    available: usize,
    max: usize,
    jobs_per_worker: usize,
    byte_limit: usize,
    work: Callback,
    done: Callback,
    ctx: *mut c_void,
) -> *mut WPool {
    let count = available.min(max);
    if count < 2 {
        return core::ptr::null_mut();
    }
    assert!(jobs_per_worker > 0);
    let job_limit = count
        .checked_mul(jobs_per_worker)
        .unwrap_or_else(|| failed("size admission window", "job count overflow"));
    let ctx = Context(ctx);
    let shared = Arc::new(WindowShared {
        state: Mutex::new(WindowState {
            jobs: VecDeque::new(),
            closed: false,
            next_sequence: 0,
            in_flight: 0,
            admission_bytes: 0,
        }),
        input_ready: Condvar::new(),
        admission_ready: Condvar::new(),
        job_limit,
        byte_limit: byte_limit as u128,
    });
    // The admission window bounds this channel and the reorder buffer together.
    let (output, results) = channel::<WindowJob>();
    let writer_shared = shared.clone();
    let writer = thread::Builder::new()
        .name("wprof_json_io".into())
        .spawn(move || {
            let mut next_sequence = 0;
            let mut pending = BTreeMap::new();
            while let Ok(job) = results.recv() {
                pending.insert(job.sequence, job);
                while let Some(job) = pending.remove(&next_sequence) {
                    unsafe { ctx.call(done, &job.job) };
                    next_sequence += 1;
                    let mut state = writer_shared.state.lock().unwrap();
                    state.in_flight -= 1;
                    state.admission_bytes -= job.admission_bytes as u128;
                    writer_shared.admission_ready.notify_one();
                }
            }
        })
        .unwrap_or_else(|e| failed("start writer", e));
    let mut workers = Vec::with_capacity(count);
    for i in 0..count {
        let shared = shared.clone();
        let output = output.clone();
        workers.push(
            thread::Builder::new()
                .name(format!("wprof_pool{i}"))
                .spawn(move || loop {
                    let mut state = shared.state.lock().unwrap();
                    while state.jobs.is_empty() && !state.closed {
                        state = shared.input_ready.wait(state).unwrap();
                    }
                    let Some(job) = state.jobs.pop_front() else {
                        break;
                    };
                    drop(state);
                    unsafe { ctx.call(work, &job.job) };
                    output
                        .send(job)
                        .unwrap_or_else(|e| failed("return completed job", e));
                })
                .unwrap_or_else(|e| failed("start worker", e)),
        );
    }
    drop(output);
    Box::into_raw(Box::new(WPool {
        mode: PoolMode::Windowed(WindowedPool {
            shared,
            workers,
            writer,
        }),
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
pub extern "C" fn wpool_new_windowed(
    max_workers: usize,
    jobs_per_worker: usize,
    byte_limit: usize,
    work: Callback,
    done: Callback,
    ctx: *mut c_void,
) -> *mut WPool {
    let available =
        thread::available_parallelism().unwrap_or_else(|e| failed("query available CPUs", e));
    new_windowed_with_cpus(
        available.get(),
        max_workers,
        jobs_per_worker,
        byte_limit,
        work,
        done,
        ctx,
    )
}

#[no_mangle]
pub unsafe extern "C" fn wpool_submit(pool: *mut WPool, job: *mut c_void) {
    wpool_submit_sized(pool, job, 0);
}

#[no_mangle]
pub unsafe extern "C" fn wpool_submit_sized(
    pool: *mut WPool,
    job: *mut c_void,
    admission_bytes: usize,
) {
    match &mut (*pool).mode {
        PoolMode::Ring(pool) => pool.submit(Job(job)),
        PoolMode::Windowed(pool) => pool.submit(Job(job), admission_bytes),
    }
}

#[no_mangle]
pub unsafe extern "C" fn wpool_finish(pool: *mut WPool) {
    if !pool.is_null() {
        match Box::from_raw(pool).mode {
            PoolMode::Ring(pool) => pool.finish(),
            PoolMode::Windowed(pool) => pool.finish(),
        }
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
        done_name: Option<String>,
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
            done_name: thread::current().name().map(str::to_owned),
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

    #[test]
    fn windowed_done_preserves_order_on_the_writer_thread() {
        let caller = thread::current().id();
        let gate = Arc::new(Gate::default());
        let mut out = Output::default();
        let pool = new_windowed_with_cpus(
            2,
            4,
            4,
            usize::MAX,
            work,
            done,
            (&mut out as *mut Output).cast(),
        );
        unsafe {
            for id in 0..8 {
                wpool_submit_sized(pool, job(id, Some(gate.clone())), 1);
            }
            wpool_finish(pool);
        }
        assert_eq!(ids(&out), (0..8).collect::<Vec<_>>());
        let work_order = gate.completed.lock().unwrap();
        assert!(
            work_order.iter().position(|&id| id == 1) < work_order.iter().position(|&id| id == 0)
        );
        for job in &out.jobs {
            assert_eq!(job.result, Some(job.id as u64 * 3 + 1));
            assert_ne!(job.worker, Some(caller));
            assert_ne!(job.done_caller, caller);
            assert_eq!(job.done_caller, out.jobs[0].done_caller);
            assert_eq!(job.done_name.as_deref(), Some("wprof_json_io"));
        }
    }

    struct WriterGate {
        entered: std::sync::mpsc::Sender<()>,
        release: Receiver<()>,
        done_count: usize,
    }

    unsafe extern "C" fn gated_done(ctx: *mut c_void, job: *mut c_void) {
        let gate = &mut *ctx.cast::<WriterGate>();
        let job = Box::from_raw(job.cast::<TestJob>());
        if job.id == 0 {
            gate.entered.send(()).unwrap();
            gate.release.recv().unwrap();
        }
        gate.done_count += 1;
    }

    #[test]
    fn windowed_admission_waits_until_done_returns_for_count_and_bytes() {
        use std::sync::mpsc::RecvTimeoutError;
        use std::time::Duration;

        for (jobs_per_worker, byte_limit, cost, initial) in [(1, usize::MAX, 0, 2), (4, 5, 6, 1)] {
            let (entered_tx, entered_rx) = channel();
            let (release_tx, release_rx) = channel();
            let (attempting_tx, attempting_rx) = channel();
            let (submitted_tx, submitted_rx) = channel();
            let submitter = thread::spawn(move || {
                let mut gate = WriterGate {
                    entered: entered_tx,
                    release: release_rx,
                    done_count: 0,
                };
                let pool = new_windowed_with_cpus(
                    2,
                    4,
                    jobs_per_worker,
                    byte_limit,
                    work,
                    gated_done,
                    (&mut gate as *mut WriterGate).cast(),
                );
                unsafe {
                    for id in 0..initial {
                        wpool_submit_sized(pool, job(id, None), cost);
                    }
                    attempting_tx.send(()).unwrap();
                    wpool_submit_sized(pool, job(initial, None), cost);
                    submitted_tx.send(()).unwrap();
                    wpool_finish(pool);
                }
                gate.done_count
            });
            entered_rx.recv().unwrap();
            attempting_rx.recv().unwrap();
            assert_eq!(
                submitted_rx.recv_timeout(Duration::from_millis(20)),
                Err(RecvTimeoutError::Timeout)
            );
            release_tx.send(()).unwrap();
            submitted_rx.recv().unwrap();
            assert_eq!(submitter.join().unwrap(), initial + 1);
        }
    }
}
