// bpf_program_fixed.c
// Fixed eBPF program with kretprobe to capture actual bytes copied
// clang -O2 -target bpf -c bpf_program_fixed.c -o bpf_program_fixed.o

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/ptrace.h>

// Unified event structure matching shared_events.h
struct event {
    __u32 type;              // EVENT_TYPE_KERNEL
    __u64 seq_no;
    __u64 ts_nic_ns;
    __u64 ts_kernel_ns;
    __u64 ts_userspace_ns;
    __s32 sock_fd;
    __u64 sock_ptr;
    __u32 tcp_seq;
    __u32 pid;
    __u32 pkt_len;
    __u32 data_len;
    __u16 src_port;
    __u16 dst_port;
    char data[512];
};

// Ring buffer for events
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);
} rb SEC(".maps");

// Map to track socket pointer for our WebSocket connection
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __s32);      // Socket FD
    __type(value, __u64);    // Socket pointer
} socket_map SEC(".maps");

// Temporary storage to correlate kprobe entry with kretprobe exit
struct recv_context {
    __u64 ts_ns;
    __u64 sock_ptr;
    __u32 pid;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);      // Thread ID
    __type(value, struct recv_context);
} recv_ctx_map SEC(".maps");

// Kprobe at entry: capture timestamp and socket pointer
SEC("kprobe/tcp_recvmsg")
int kprobe__tcp_recvmsg(struct pt_regs *ctx) {
    __u64 tid = bpf_get_current_pid_tgid();
    __u32 pid = tid >> 32;
    
    struct recv_context rctx = {};
    rctx.ts_ns = bpf_ktime_get_ns();
    rctx.sock_ptr = (unsigned long)PT_REGS_PARM1(ctx);
    rctx.pid = pid;
    
    bpf_map_update_elem(&recv_ctx_map, &tid, &rctx, BPF_ANY);
    
    return 0;
}

// Kretprobe at exit: capture actual bytes copied (return value)
SEC("kretprobe/tcp_recvmsg")
int kretprobe__tcp_recvmsg(struct pt_regs *ctx) {
    __u64 tid = bpf_get_current_pid_tgid();
    __u32 pid = tid >> 32;
    
    // Retrieve context from entry probe
    struct recv_context *rctx = bpf_map_lookup_elem(&recv_ctx_map, &tid);
    if (!rctx) {
        return 0;
    }
    
    // Get return value (bytes copied, or negative error)
    long bytes_copied = PT_REGS_RC(ctx);
    if (bytes_copied <= 0) {
        bpf_map_delete_elem(&recv_ctx_map, &tid);
        return 0;
    }
    
    // Check if this socket is our WebSocket connection
    // (In production, filter by checking socket_map)
    
    struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&recv_ctx_map, &tid);
        return 0;
    }
    
    e->type = 2;  // EVENT_TYPE_KERNEL
    e->seq_no = 0;
    e->ts_nic_ns = 0;
    e->ts_kernel_ns = rctx->ts_ns;
    e->ts_userspace_ns = 0;
    e->sock_fd = -1;
    e->sock_ptr = rctx->sock_ptr;
    e->tcp_seq = 0;
    e->pid = rctx->pid;
    e->pkt_len = (__u32)bytes_copied;
    e->data_len = 0;
    e->src_port = 0;
    e->dst_port = 0;
    
    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&recv_ctx_map, &tid);
    
    return 0;
}

// Helper program to update socket_map from userspace
SEC("raw_tracepoint/sys_enter")
int trace_socket_update(struct pt_regs *ctx) {
    // This would be called by userspace to register the socket FD
    // when WebSocket connection is established
    return 0;
}

char LICENSE[] SEC("license") = "GPL";