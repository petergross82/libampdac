#include "ampdac_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *msg) {
    fprintf(stderr, "ipc_smoke: %s (%s)\n", msg, ampdac_client_last_error());
    return 1;
}

int main(int argc, char **argv) {
    const char *daemon_bin = argc > 1 ? argv[1] : "./build/ampdacd";
    const char *sock = "/tmp/ampdac-smoke.sock";
    unlink(sock);

    const pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        execl(daemon_bin, "ampdacd", "--socket", sock, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    ampdac_client *held = NULL;
    for (int i = 0; i < 50; ++i) {
        held = ampdac_client_connect(sock);
        if (held != NULL) break;
        usleep(50000);
    }
    if (held == NULL) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return fail("daemon did not accept a connection");
    }

    ampdac_device_info devices[8];
    const int listed = ampdac_client_list_devices(sock, devices, 8);
    if (listed < 0) {
        ampdac_client_disconnect(held);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return fail("list");
    }
    printf("ipc_smoke: list -> %d device(s)\n", listed);

    ampdac_pcm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate_hz = 44100;
    cfg.channels = 2;
    cfg.format = AMPDAC_PCM_S16LE;
    cfg.volume = 1.0f;
    const ampdac_status st = ampdac_client_configure_pcm(held, 0xFFFF, 0xFFFF, &cfg);
    if (st != AMPDAC_ERR_NO_DEVICE) {
        fprintf(stderr, "ipc_smoke: configure status %d, want NO_DEVICE\n", (int)st);
        ampdac_client_disconnect(held);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }
    printf("ipc_smoke: missing dac -> %s\n", ampdac_client_last_error());

    ampdac_client *second = ampdac_client_connect(sock);
    if (second == NULL) {
        ampdac_client_disconnect(held);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return fail("second connect");
    }
    const ampdac_status busy = ampdac_client_configure_pcm(second, 0xFFFF, 0xFFFF, &cfg);
    if (busy != AMPDAC_ERR_BUSY) {
        fprintf(stderr, "ipc_smoke: second configure status %d, want BUSY\n", (int)busy);
        ampdac_client_disconnect(second);
        ampdac_client_disconnect(held);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }
    printf("ipc_smoke: second client -> busy\n");
    ampdac_client_disconnect(second);
    ampdac_client_disconnect(held);

    if (kill(pid, SIGTERM) != 0) return fail("kill");
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ipc_smoke: daemon status %d\n", status);
        return 1;
    }
    printf("ipc_smoke: ok\n");
    return 0;
}
