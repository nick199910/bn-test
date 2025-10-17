// bpf_program.c
// clang -O2 -target bpf -c bpf_program.c -o bpf_program.o
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h> 
#include <socket.h>
#include <tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/ptrace.h>

struct event {
    __u64 ts_ns;
    __u64 sockptr;
    __u32 copied; // bytes copied to user
    __u32 pid;
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);
} rb SEC(".maps");

SEC("kprobe/tcp_recvmsg")
int kprobe__tcp_recvmsg(struct pt_regs *ctx) {
    struct event *e;
    e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e) {
        return 0;
    }
    e->ts_ns = bpf_ktime_get_ns();
    e->sockptr = (unsigned long)PT_REGS_PARM1(ctx); // sock* in first arg
    // second arg is iov, third is size — for simplicity capture size passed
    e->copied = (unsigned int)PT_REGS_PARM3(ctx);
    e->pid = bpf_get_current_pid_tgid() >> 32;
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
