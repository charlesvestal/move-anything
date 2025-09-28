#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

unsigned char *global_mmap_addr = NULL;
FILE *output_file;
int frame_counter = 0;

void print_mem()
{
    printf("\033[H\033[J");
    for (int i = 0; i < 4096; ++i)
    {
        printf("%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            printf("\n\n");
        }
    }
    printf("\n\n");
}

void write_mem()
{
    if (!output_file)
    {
        return;
    }

    // printf("\033[H\033[J");
    fprintf(output_file, "--------------------------------------------------------------------------------------------------------------");
    fprintf(output_file, "Frame: %d\n", frame_counter);
    for (int i = 0; i < 4096; ++i)
    {
        fprintf(output_file, "%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            fprintf(output_file, "\n\n");
        }
    }
    fprintf(output_file, "\n\n");

    sync();

    frame_counter++;
}

void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{

    printf(">>>>>>>>>>>>>>>>>>>>>>>> Hooked mmap...\n");
    if (!real_mmap)
    {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
        if (!real_mmap)
        {
            fprintf(stderr, "Error: dlsym failed to find mmap\n");
            exit(1);
        }
    }

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    if (length == 4096)
    {
        global_mmap_addr = result;
    }

    printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%ld, result=%p\n",
           addr, length, prot, flags, fd, offset, result);

    // output_file = fopen("spi_memory.txt", "w+");

    return result;
}

void launchChildAndKillThisProcess(char *pBinPath, char*pBinName, char* pArgs)
{
    int pid = fork();

    if (pid < 0)
    {
        printf("Fork failed\n");
        exit(1);
    }
    else if (pid == 0)
    {
        // Child process
        setsid();
        // Perform detached task
        printf("Child process running in the background...\n");

        printf("Args: %s\n", pArgs);

        // Close all file descriptors, otherwise /dev/ablspi0.0 is held open
        // and the control surface code can't open it.
        printf("Closing file descriptors...\n");
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++)
        {
            close(i);
        }

        // Let's a go!
        int ret = execl(pBinPath, pBinName, pArgs, (char *)0);
    }
    else
    {
        // parent
        kill(getpid(), SIGINT);
    }
}

int (*real_ioctl)(int, unsigned long, char *) = NULL;

int shiftHeld = 0;
int volumeTouched = 0;
int wheelTouched = 0;
int launchInProgress = 0;
struct timespec volumeTouchedTs = {0};
struct timespec wheelTouchedTs = {0};

static long diff_ms(struct timespec *a, struct timespec *b)
{
    long sec = a->tv_sec - b->tv_sec;
    long nsec = a->tv_nsec - b->tv_nsec;
    return sec * 1000 + nsec / 1000000;
}
void midi_monitor()
{
    int startByte = 2048;
    int length = 256;
    int endByte = startByte + length;

    for (int i = startByte; i < endByte; i += 4)
    {
        if ((unsigned int)global_mmap_addr[i] == 0)
        {
            continue;
        }

        unsigned char *byte = &global_mmap_addr[i];
        unsigned char cable = (*byte & 0b11110000) >> 4;
        unsigned char code_index_number = (*byte & 0b00001111);
        unsigned char midi_0 = *(byte + 1);
        unsigned char midi_1 = *(byte + 2);
        unsigned char midi_2 = *(byte + 3);

        if (code_index_number == 2 || code_index_number == 1 || (cable == 0xf && code_index_number == 0xb && midi_0 == 176))
        {
            continue;
        }

        if (midi_0 + midi_1 + midi_2 == 0)
        {
            continue;
        }

        int controlMessage = 0xb0;
        if (midi_0 == controlMessage)
        {
            printf("Control message\n");

            if (midi_1 == 0x31)
            {
                if (midi_2 == 0x7f)
                {
                    printf("Shift on\n");

                    shiftHeld = 1;
                }
                else
                {
                    printf("Shift off\n");

                    shiftHeld = 0;
                }
            }

            int leftArrow = 62;
            if (midi_1 == leftArrow)
            {
                if (midi_2 == 0x7f)
                {
                    if (shiftHeld)
                    {
                        printf("/data/UserData/control_surface_move/changePageRelative.sh -1\n");
                        launchChildAndKillThisProcess("/data/UserData/control_surface_move/changePageRelative.sh", "changePageRelative.sh", "-1");
                    }
                }
            }

            int rightArrow = 63;
            if (midi_1 == rightArrow)
            {
                if (midi_2 == 0x7f)
                {
                    if (shiftHeld)
                    {
                        printf("/data/UserData/control_surface_move/changePageRelative.sh 1\n");
                        launchChildAndKillThisProcess("/data/UserData/control_surface_move/changePageRelative.sh", "changePageRelative.sh", "1");
                    }
                }
            }
        }

        if ((midi_0 == 0x90 || midi_0 == 0x80) && midi_1 == 0x08)
        {
            if (midi_0 == 0x90 && midi_2 == 0x7f)
            {
                volumeTouched = 1;
                clock_gettime(CLOCK_MONOTONIC, &volumeTouchedTs);
            }
            else if (midi_0 == 0x80 || midi_2 == 0x00)
            {
                volumeTouched = 0;
            }
        }

        if ((midi_0 == 0x90 || midi_0 == 0x80) && midi_1 == 0x09)
        {
            if (midi_0 == 0x90 && midi_2 == 0x7f)
            {
                wheelTouched = 1;
                clock_gettime(CLOCK_MONOTONIC, &wheelTouchedTs);
            }
            else if (midi_0 == 0x80 || midi_2 == 0x00)
            {
                wheelTouched = 0;
            }
        }

        if (!launchInProgress && volumeTouched && wheelTouched)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (diff_ms(&now, &volumeTouchedTs) > 200 && diff_ms(&now, &wheelTouchedTs) > 200)
            {
                launchInProgress = 1;
                printf("Toggling control surface!\n");
                launchChildAndKillThisProcess("/data/UserData/control_surface_move/start_control_surface_move.sh", "start_control_surface_move.sh", "");
            }
        }

        printf("control_surface_move: cable: %x,\tcode index number:%x,\tmidi_0:%x,\tmidi_1:%x,\tmidi_2:%x\n", cable, code_index_number, midi_0, midi_1, midi_2);
    }
}

// unsigned long ioctlCounter = 0;
int ioctl(int fd, unsigned long request, char *argp)
{
    if (!real_ioctl)
    {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
        {
            fprintf(stderr, "Error: dlsym failed to find ioctl\n");
            exit(1);
        }
    }

    // print_mem();
    // write_mem();

    // Shoudl probably just change this to use the control_surface_move code and quickjs for flexibility
    midi_monitor();

    int result = real_ioctl(fd, request, argp);

    return result;
}
