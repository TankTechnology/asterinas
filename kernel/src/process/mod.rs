// SPDX-License-Identifier: MPL-2.0

mod clone;
pub mod credentials;
mod exit;
mod kill;
pub mod posix_thread;
#[expect(clippy::module_inception)]
mod process;
mod process_filter;
pub mod process_table;
mod process_vm;
mod program_loader;
pub mod rlimit;
pub mod signal;
mod status;
pub mod sync;
mod task_set;
mod term_status;
mod wait;
mod process_ctx_id;

pub use clone::{clone_child, CloneArgs, CloneFlags};
pub use credentials::{Credentials, Gid, Uid};
pub use kill::{kill, kill_all, kill_group, tgkill};
pub use process::{
    ExitCode, JobControl, Pgid, Pid, Process, ProcessBuilder, ProcessGroup, Session, Sid, Terminal,
};
pub use process_ctx_id::{allocate_process_ctx_id, last_process_ctx_id, ProcessCtxId, ProcessCtxIdManager, process_ctx_id_manager};
pub use process_filter::ProcessFilter;
pub use process_vm::{MAX_ARGV_NUMBER, MAX_ARG_LEN, MAX_ENVP_NUMBER, MAX_ENV_LEN};
pub use program_loader::{check_executable_file, load_program_to_vm};
pub use rlimit::ResourceType;
pub use term_status::TermStatus;
pub use wait::{wait_child_exit, WaitOptions};

pub(super) fn init() {
    process::init();
    posix_thread::futex::init();
    process_ctx_id::init();
}
