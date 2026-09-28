//! Interrupting a turn: Ctrl-C (SIGINT) or Esc stops the model's answer or the running tool and
//! returns to the prompt, as in Claude Code.
//!
//! SIGINT is caught without SA_RESTART, so a read blocked on the host's request API returns
//! EINTR and the reader (bridge.rs) gives up. Helper threads block SIGINT, so the kernel always
//! delivers it to the main thread, which is the one that waits. Esc, read by the key watcher
//! thread, sends SIGINT to the main thread the same way.
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

static INTERRUPTED: AtomicBool = AtomicBool::new(false);
static MAIN_THREAD: AtomicUsize = AtomicUsize::new(0);
static INSTALLED: AtomicBool = AtomicBool::new(false);
static BACKGROUND: AtomicBool = AtomicBool::new(false);

/// Ctrl-B while a command runs: move it to the background.
pub fn request_background() {
    BACKGROUND.store(true, Ordering::SeqCst);
}

pub fn take_background() -> bool {
    BACKGROUND.swap(false, Ordering::SeqCst)
}

extern "C" fn on_sigint(_: libc::c_int) {
    INTERRUPTED.store(true, Ordering::SeqCst);
}

/// Catches SIGINT for the whole process (interactive mode). Call from the main thread.
pub fn install() {
    // SAFETY: a plain signal handler that only stores an atomic; pthread_self is always valid.
    unsafe {
        MAIN_THREAD.store(libc::pthread_self() as usize, Ordering::SeqCst);
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = on_sigint as *const () as usize;
        libc::sigemptyset(&mut action.sa_mask);
        action.sa_flags = 0;
        libc::sigaction(libc::SIGINT, &action, std::ptr::null_mut());
    }
    INSTALLED.store(true, Ordering::SeqCst);
}

pub fn installed() -> bool {
    INSTALLED.load(Ordering::SeqCst)
}

/// For threads other than the main one: SIGINT is not theirs to take.
pub fn block_in_this_thread() {
    // SAFETY: sigset manipulation and pthread_sigmask on this thread.
    unsafe {
        let mut set: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut set);
        libc::sigaddset(&mut set, libc::SIGINT);
        libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
    }
}

/// Interrupts the main thread (Esc pressed): the flag, and SIGINT so a blocked read returns.
pub fn trigger() {
    INTERRUPTED.store(true, Ordering::SeqCst);
    let main = MAIN_THREAD.load(Ordering::SeqCst);
    if main != 0 && installed() {
        // SAFETY: the main thread's id, recorded by install(); it lives as long as the process.
        unsafe { libc::pthread_kill(main as libc::pthread_t, libc::SIGINT) };
    }
}

pub fn is_set() -> bool {
    INTERRUPTED.load(Ordering::SeqCst)
}

pub fn clear() {
    INTERRUPTED.store(false, Ordering::SeqCst);
}

/// Takes the flag: true once per interrupt.
pub fn take() -> bool {
    INTERRUPTED.swap(false, Ordering::SeqCst)
}

/// A thread whose SIGINT is blocked from the start, returning a value.
pub fn spawn_returning<T: Send + 'static, F: FnOnce() -> T + Send + 'static>(work: F) -> std::thread::JoinHandle<T> {
    std::thread::spawn(move || {
        block_in_this_thread();
        work()
    })
}

/// A thread whose SIGINT is blocked from the start.
pub fn spawn<F: FnOnce() + Send + 'static>(work: F) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        block_in_this_thread();
        work()
    })
}
