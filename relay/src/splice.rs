// ============================================================================
// SPLICE — Zero-copy kernel relay using splice() + tee()
//
// The hot path: NIC → kernel buffer → pipe → N output sockets
// CPU never touches the frame data. It only reads the 4-byte length prefix.
//
// splice(): moves data between a file descriptor and a pipe (kernel-only)
// tee():    duplicates pipe data without consuming it (kernel-only copy)
//
// For raw TCP clients only. WebSocket clients need userspace framing.
// ============================================================================

use nix::fcntl::{splice, tee, SpliceFFlags, fcntl, FcntlArg, OFlag};
use std::os::unix::io::{AsRawFd, BorrowedFd, RawFd};
use tracing::{debug, warn};

/// Create a pipe pair (read_fd, write_fd) as raw fds
pub fn create_pipe() -> std::io::Result<(RawFd, RawFd)> {
    let (r, w) = nix::unistd::pipe()?;
    let r_raw = r.as_raw_fd();
    let w_raw = w.as_raw_fd();
    set_nonblock(r_raw)?;
    set_nonblock(w_raw)?;
    // Leak OwnedFds — we manage these raw fds ourselves
    std::mem::forget(r);
    std::mem::forget(w);
    Ok((r_raw, w_raw))
}

fn set_nonblock(fd: RawFd) -> std::io::Result<()> {
    let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
    let flags = fcntl(&borrowed, FcntlArg::F_GETFL).map_err(std::io::Error::from)?;
    let flags = OFlag::from_bits_truncate(flags) | OFlag::O_NONBLOCK;
    fcntl(&borrowed, FcntlArg::F_SETFL(flags)).map_err(std::io::Error::from)?;
    Ok(())
}

/// Splice from socket to pipe (zero-copy read from network)
pub fn splice_in(sock_fd: RawFd, pipe_write: RawFd, len: usize) -> std::io::Result<usize> {
    let sock = unsafe { BorrowedFd::borrow_raw(sock_fd) };
    let pipe = unsafe { BorrowedFd::borrow_raw(pipe_write) };
    match splice(
        &sock,
        None,
        &pipe,
        None,
        len,
        SpliceFFlags::SPLICE_F_MOVE | SpliceFFlags::SPLICE_F_NONBLOCK,
    ) {
        Ok(n) => Ok(n),
        Err(nix::errno::Errno::EAGAIN) => Ok(0),
        Err(e) => Err(std::io::Error::from(e)),
    }
}

/// Splice from pipe to socket (zero-copy write to network)
pub fn splice_out(pipe_read: RawFd, sock_fd: RawFd, len: usize) -> std::io::Result<usize> {
    let pipe = unsafe { BorrowedFd::borrow_raw(pipe_read) };
    let sock = unsafe { BorrowedFd::borrow_raw(sock_fd) };
    match splice(
        &pipe,
        None,
        &sock,
        None,
        len,
        SpliceFFlags::SPLICE_F_MOVE | SpliceFFlags::SPLICE_F_NONBLOCK,
    ) {
        Ok(n) => Ok(n),
        Err(nix::errno::Errno::EAGAIN) => Ok(0),
        Err(e) => Err(std::io::Error::from(e)),
    }
}

/// Tee: duplicate pipe content to another pipe without consuming
pub fn tee_pipe(src_read: RawFd, dst_write: RawFd, len: usize) -> std::io::Result<usize> {
    let src = unsafe { BorrowedFd::borrow_raw(src_read) };
    let dst = unsafe { BorrowedFd::borrow_raw(dst_write) };
    match tee(
        &src,
        &dst,
        len,
        SpliceFFlags::SPLICE_F_NONBLOCK,
    ) {
        Ok(n) => Ok(n),
        Err(nix::errno::Errno::EAGAIN) => Ok(0),
        Err(e) => Err(std::io::Error::from(e)),
    }
}

/// Close a raw fd safely
pub fn close_fd(fd: RawFd) {
    let _ = nix::unistd::close(fd);
}

/// Set pipe buffer size (Linux-specific)
pub fn set_pipe_size(fd: RawFd, size: i32) -> std::io::Result<()> {
    let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
    match fcntl(&borrowed, FcntlArg::F_SETPIPE_SZ(size)) {
        Ok(_) => {
            debug!("Pipe buffer set to {} bytes", size);
            Ok(())
        }
        Err(e) => {
            warn!("Failed to set pipe size to {}: {} (using default)", size, e);
            Ok(())
        }
    }
}
