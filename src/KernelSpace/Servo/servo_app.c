#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>

#ifndef __KERNEL__
#include <linux/ioctl.h>
#endif

#define SERVO_IOC_MAGIC 'S'

struct servo_sweep_cmd {
    int start_angle;
    int end_angle;
    int step;
    int delay_ms;
};

#define SERVO_IOCTL_SET_ANGLE  _IOW(SERVO_IOC_MAGIC, 1, int)
#define SERVO_IOCTL_GET_ANGLE  _IOR(SERVO_IOC_MAGIC, 2, int)
#define SERVO_IOCTL_SWEEP      _IOW(SERVO_IOC_MAGIC, 3, struct servo_sweep_cmd)
#define SERVO_IOCTL_CENTER     _IO (SERVO_IOC_MAGIC, 4)

#define DEVICE "/dev/servo"

static int open_device(int flags)
{
    int fd = open(DEVICE, flags);
    if (fd < 0) {
        perror("open " DEVICE);
        exit(EXIT_FAILURE);
    }
    return fd;
}

static void set_angle_write(int fd, int angle)
{
    char buf[16];
    int  len = snprintf(buf, sizeof(buf), "%d\n", angle);
    if (write(fd, buf, len) < 0)
        perror("write");
    else
        printf("[write] Sent angle: %d\n", angle);
}

static void set_angle_ioctl(int fd, int angle)
{
    if (ioctl(fd, SERVO_IOCTL_SET_ANGLE, &angle) < 0)
        perror("ioctl SET_ANGLE");
    else
        printf("[ioctl] Set angle: %d\n", angle);
}

static int get_angle_ioctl(int fd)
{
    int angle = -1;
    if (ioctl(fd, SERVO_IOCTL_GET_ANGLE, &angle) < 0)
        perror("ioctl GET_ANGLE");
    return angle;
}

typedef struct { int fd; int max_reads; } reader_args_t;

static void *reader_thread(void *arg)
{
    reader_args_t *a  = (reader_args_t *)arg;
    char           buf[32];
    int            n, i;

    printf("[reader_thread] started, will do %d blocking reads\n", a->max_reads);
    for (i = 0; i < a->max_reads; i++) {
        n = read(a->fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EINTR) break;
            perror("read");
            break;
        }
        buf[n] = '\0';
        printf("[reader_thread] angle change -> %s", buf);
    }
    printf("[reader_thread] done\n");
    return NULL;
}

typedef struct { int start; int end; int step; int delay_ms; } sweep_args_t;

static void *sweep_thread(void *arg)
{
    sweep_args_t *a = (sweep_args_t *)arg;
    int  fd = open_device(O_WRONLY);
    char buf[64];
    int  len;

    printf("[sweep_thread] sweep %d->%d step=%d delay=%dms\n",
           a->start, a->end, a->step, a->delay_ms);

    len = snprintf(buf, sizeof(buf), "sweep %d %d %d %d\n",
                   a->start, a->end, a->step, a->delay_ms);

    if (write(fd, buf, len) < 0)
        perror("write sweep");
    else
        printf("[sweep_thread] sweep complete\n");

    close(fd);
    return NULL;
}

static void interactive_mode(void)
{
    int  fd = open_device(O_RDWR);
    char line[128];

    printf("\n=== Servo Interactive Mode ===\n");
    printf("Commands:\n");
    printf("  <angle>              : set angle 0-180\n");
    printf("  sweep <s> <e> <step> : sweep with 20ms delay\n");
    printf("  center               : go to 90\n");
    printf("  get                  : read current angle\n");
    printf("  stats                : show /proc/servo_stats\n");
    printf("  quit                 : exit\n\n");

    while (1) {
        int angle;
        printf("servo> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0)
            break;

        if (strcmp(line, "center") == 0) {
            ioctl(fd, SERVO_IOCTL_CENTER);
            printf("Centred at 90\n");
        } else if (strcmp(line, "get") == 0) {
            printf("Current angle: %d\n", get_angle_ioctl(fd));
        } else if (strcmp(line, "stats") == 0) {
            system("cat /proc/servo_stats");
        } else if (strncmp(line, "sweep ", 6) == 0) {
            struct servo_sweep_cmd sw = {0, 180, 5, 20};
            sscanf(line + 6, "%d %d %d", &sw.start_angle, &sw.end_angle, &sw.step);
            sw.delay_ms = 20;
            if (ioctl(fd, SERVO_IOCTL_SWEEP, &sw) < 0)
                perror("ioctl SWEEP");
            else
                printf("Sweep complete\n");
        } else if (sscanf(line, "%d", &angle) == 1) {
            set_angle_ioctl(fd, angle);
        } else if (strlen(line) > 0) {
            printf("Unknown command: %s\n", line);
        }
    }

    close(fd);
}

static void demo_mode(void)
{
    int          fd_rw, fd_read;
    pthread_t    reader_tid, sweep_tid;
    reader_args_t rargs;
    sweep_args_t  sargs;
    pid_t         pid;

    printf("\n=== Servo Driver Demo ===\n\n");

    fd_rw   = open_device(O_RDWR);
    fd_read = open_device(O_RDONLY);

    printf("--- Part 1: Basic angle writes ---\n");
    set_angle_write(fd_rw, 0);    sleep(1);
    set_angle_write(fd_rw, 90);   sleep(1);
    set_angle_write(fd_rw, 180);  sleep(1);
    set_angle_write(fd_rw, 90);   sleep(1);

    printf("\n--- Part 2: ioctl commands ---\n");
    set_angle_ioctl(fd_rw, 45);   sleep(1);
    printf("[ioctl] Current angle: %d\n", get_angle_ioctl(fd_rw));
    ioctl(fd_rw, SERVO_IOCTL_CENTER);
    printf("[ioctl] Centred\n");   sleep(1);

    printf("\n--- Part 3: Concurrent blocking read + sweep (threads) ---\n");
    rargs.fd        = fd_read;
    rargs.max_reads = 10;
    pthread_create(&reader_tid, NULL, reader_thread, &rargs);

    sargs.start    = 0;
    sargs.end      = 180;
    sargs.step     = 20;
    sargs.delay_ms = 150;
    pthread_create(&sweep_tid, NULL, sweep_thread, &sargs);

    pthread_join(sweep_tid, NULL);
    pthread_cancel(reader_tid);
    pthread_join(reader_tid, NULL);

    printf("\n--- Part 4: Multi-process demo (fork) ---\n");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        int cfd = open_device(O_RDONLY);
        char cbuf[32];
        int  n, count = 0;
        printf("[child  pid=%d] waiting for angle updates...\n", getpid());
        while (count < 5) {
            n = read(cfd, cbuf, sizeof(cbuf) - 1);
            if (n <= 0) break;
            cbuf[n] = '\0';
            printf("[child  pid=%d] angle -> %s", getpid(), cbuf);
            count++;
        }
        close(cfd);
        exit(0);
    } else if (pid > 0) {
        int angles[] = {30, 60, 90, 120, 150};
        int i;
        printf("[parent pid=%d] writing angles...\n", getpid());
        for (i = 0; i < 5; i++) {
            usleep(300000);
            set_angle_ioctl(fd_rw, angles[i]);
        }
        wait(NULL);
        printf("[parent] child exited\n");
    } else {
        perror("fork");
    }

    printf("\n--- Part 5: /proc/servo_stats ---\n");
    system("cat /proc/servo_stats");

    close(fd_read);
    close(fd_rw);
    printf("\nDemo complete.\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s --demo | --interactive | --angle <deg> | "
            "--sweep <start> <end> <step>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--demo") == 0) {
        demo_mode();
    } else if (strcmp(argv[1], "--interactive") == 0) {
        interactive_mode();
    } else if (strcmp(argv[1], "--angle") == 0 && argc >= 3) {
        int angle = atoi(argv[2]);
        int fd    = open_device(O_WRONLY);
        set_angle_ioctl(fd, angle);
        close(fd);
    } else if (strcmp(argv[1], "--sweep") == 0 && argc >= 5) {
        struct servo_sweep_cmd sw;
        int fd = open_device(O_RDWR);
        sw.start_angle = atoi(argv[2]);
        sw.end_angle   = atoi(argv[3]);
        sw.step        = atoi(argv[4]);
        sw.delay_ms    = (argc >= 6) ? atoi(argv[5]) : 20;
        if (ioctl(fd, SERVO_IOCTL_SWEEP, &sw) < 0)
            perror("ioctl SWEEP");
        else
            printf("Sweep %d->%d done\n", sw.start_angle, sw.end_angle);
        close(fd);
    } else {
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
