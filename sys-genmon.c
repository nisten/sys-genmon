#if 0
TMP="$(mktemp -d)"; cc -o "$TMP/a.out" -x c "$0" && "$TMP/a.out" $@; RVAL=$?; rm -rf "$TMP"; exit $RVAL
#endif

// Requires linux 2.6.33 (Released Feb 2010) or later.
// Assumes that the number of CPUs doesn't change while the program is running.

// Statistics that I care about:
// --------------------------------
// 1. CPU usage - all cores - Blue, alternating
// 2. Memory usage - Yellow
// 3. Swap usage - Purple
// --------------------------------
// 4. GPU utilization % - Green
// 5. Vram usage -
// --------------------------------
// 6. CPU temp - Red
// 7. GPU temp - Orange
// --------------------------------
// 8. Disk usage
// 9. Network usage.
// --------------------------------

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_NUM_CPUS 256
#define MAX_NUM_GPUS 8

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"

#define CPU_COLORS "#3498DB", "#2471A3"
#define GPU_COLORS "#76B900", "#27AE60"
#define MEM_COLOR "#F1C40F"
#define SWP_COLOR "#8E44AD"
#define VRAM_COLOR "#BADC00"

#define PAGE_SIZE 4096

struct all_info {
  struct cpu_record {
    struct cpu_instance {
      char cpu_number[16];
      uint32_t user;
      uint32_t system;

      uint32_t idle;
      uint32_t iowait;
      uint32_t irq;
      uint32_t softirq;

      uint32_t steal;
      uint32_t guest;
    } cpu[MAX_NUM_CPUS];
    size_t num_cpus;
  } cpu_info;

  struct gpu_record {
    struct gpu_instance {
      char gpu_name[256];
      uint32_t gpu_sm_utilization;
      uint32_t gpu_mem_bandwidth_utilization;

      float gpu_mem_used_percentage;
      uint32_t gpu_mem_total;
      uint32_t gpu_mem_used;
      uint32_t gpu_mem_free;

      uint32_t gpu_graphics_clock;
      uint32_t gpu_mem_clock;
      uint32_t gpu_video_clock;

      uint32_t gpu_power_draw;
      uint32_t gpu_temp;
    } gpu[MAX_NUM_GPUS];
    size_t num_gpus;
  } gpu_info;

  struct mem_record {
    float mem_percentage;
    uint32_t mem_total;
    uint32_t mem_used;
    uint32_t mem_free;

    float swp_percentage;
    uint32_t swp_total;
    uint32_t swp_used;
    uint32_t swp_free;
  } mem_info;
} info;

static float avg_utilization;
static float utilization[MAX_NUM_CPUS];
typedef struct cpu_record cpu_record;
typedef struct gpu_record gpu_record;
typedef struct mem_record mem_record;

static struct cpu_record *prev_cpu_info = NULL;
static char tmp_svg[512] = {0};  // Dynamic path per user
static char shm_name[256] = {0}; // Dynamic name per user
static const char *nvsmi_cmd = "nvidia-smi "
                               "--query-gpu="
                               "gpu_name,"
                               "utilization.gpu,"
                               "utilization.memory,"
                               "memory.total,"
                               "memory.used,"
                               "memory.free,"
                               "clocks.current.graphics,"
                               "clocks.current.memory,"
                               "clocks.current.video,"
                               "power.draw,"
                               "temperature.gpu "
                               "--format=csv,noheader,nounits";

// M1/Asahi GPU monitoring - Direct kernel interface, Carmack-style
// Currently stubs - waiting for DRM fdinfo support in kernel 6.16+
static inline int detect_asahi_gpu(void) {
  // Check if Asahi GPU device exists (works on all M1/M2/M3)
  struct stat st;
  // M1/M2 GPU at 0x206400000, M3 at different address
  if (stat("/sys/devices/platform/soc/206400000.gpu", &st) == 0) {
    return 1; // M1/M2 detected
  }
  // Check for DRM card0 with apple driver as fallback
  FILE *f = fopen("/sys/class/drm/card0/device/driver", "r");
  if (f) {
    fclose(f);
    return 1;
  }
  return 0;
}

static inline void get_asahi_gpu_info(struct gpu_record *gpu) {
  gpu->num_gpus = 0;

  if (!detect_asahi_gpu()) return;

  // M1 GPU detected
  gpu->num_gpus = 1;
  struct gpu_instance *g = &gpu->gpu[0];

  // Read GPU name from dmesg or default to M1
  strncpy(g->gpu_name, "Apple M1 GPU (7-core)", sizeof(g->gpu_name) - 1);
  g->gpu_name[sizeof(g->gpu_name) - 1] = '\0';

  // STUBBED: DRM fdinfo not yet available in Asahi driver v0.0.0
  // When kernel 6.16+ DRM stats land, parse /proc/[pid]/fdinfo/[fd]:
  //   drm-engine-gfx: [cycles]
  //   drm-cycles-gfx: [total]
  //   drm-memory-vram: [bytes]
  // For now: report 0 (unknown)
  g->gpu_sm_utilization = 0;  // TODO: parse DRM fdinfo when available
  g->gpu_mem_bandwidth_utilization = 0;

  // M1 GPU memory is shared with system RAM - estimate from GL apps
  g->gpu_mem_total = 0;  // Shared memory architecture
  g->gpu_mem_used = 0;
  g->gpu_mem_free = 0;
  g->gpu_mem_used_percentage = 0.0;

  // Clock/power stats not exposed by firmware
  g->gpu_graphics_clock = 0;
  g->gpu_mem_clock = 0;
  g->gpu_video_clock = 0;
  g->gpu_power_draw = 0;
  g->gpu_temp = 0;
}

static inline uint32_t str_to_u32(char *s, int *err) {
#if UINT32_MAX <= ULONG_MAX
#define strtot unsigned long
#define strtou strtoul
#elif UINT32_MAX <= ULLONG_MAX
#define strtot unsigned long long
#define strtou strtoull
#endif
  errno = 0;
  strtot ul = strtou(s, NULL, 10);
  if (errno || (ul > UINT32_MAX))
    *err = 1;
  return ul;
}

static inline int starts_with(char *s, char *start) {
  while (*start && *s) {
    if (*start != *s)
      return 0;
    start++;
    s++;
  }
  return 1;
}

static inline void init_secure_paths(void) {
  // Use XDG_RUNTIME_DIR if available, otherwise fall back to /tmp with user ID
  const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
  uid_t uid = getuid();

  if (runtime_dir && runtime_dir[0] == '/') {
    snprintf(tmp_svg, sizeof(tmp_svg), "%s/sys-genmon-%d.svg", runtime_dir, uid);
  } else {
    snprintf(tmp_svg, sizeof(tmp_svg), "/tmp/sys-genmon-%d.svg", uid);
  }

  snprintf(shm_name, sizeof(shm_name), "/genmon_shmem_%d", uid);
}

static inline char *get_cpu_name(void) {
  static char cpu_name[4096];
  if (cpu_name[0])
    return cpu_name;

  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    puts("Failed to open /proc/cpuinfo."), exit(1);

// Read the first few thousand bytes. The CPU name should be in there.
#define CPU_NAME_BUFSZ 4096
  ssize_t n_read = fread(cpu_name, CPU_NAME_BUFSZ, 1, f);
  if (n_read == -1)
    puts("Failed to read /proc/cpuinfo."), exit(1);

  fclose(f);

  // Find model name field, skip to content.
  // If it can't be found, return "Unknown CPU".
  char *p = strstr(cpu_name, "model name");
  if (!p) {
    sprintf(cpu_name, "Unknown CPU");
    return cpu_name;
  }

  // Bounded search for colon
  char *end = cpu_name + CPU_NAME_BUFSZ;
  while (*p != ':' && p < end)
    p++;
  if (p >= end) {
    sprintf(cpu_name, "Unknown CPU");
    return cpu_name;
  }
  p += 2;

  // Get size of content, overwrite with bounds checking
  char *q = p;
  while (*q != '\n' && q < end)
    q++;
  int n = q - p;
  if (n > 0 && n < CPU_NAME_BUFSZ) {
    memmove(cpu_name, p, n);
    cpu_name[n] = '\0';
  } else {
    sprintf(cpu_name, "Unknown CPU");
  }

  return cpu_name;
}

static inline char *read_memitem(char *p, char *end, char *title, uint32_t *item, int *hit_one) {
  if (!*p || p >= end) {
    puts("Failed to parse /proc/meminfo. Ran out of input."), exit(1);
  } else if (starts_with(p, title)) {
    p += strlen(title);
    while (*p == ' ' && p < end)
      p++;
    char *itemstr = p;
    while (*p != ' ' && p < end)
      p++;
    if (*p != '\n' && p < end)
      *p++ = '\0';
    while (*p != '\n' && p < end)
      p++;
    if (p < end)
      *p++ = '\0';

    int err = 0;
    *item = str_to_u32(itemstr, &err);
    if (err)
      puts("Failed to parse /proc/meminfo. Invalid number format."), exit(1);
    else
      *hit_one = 1;
  }
  return p;
}

static inline char *next_gpu_item(char *line, char *end) {
  while (line < end) {
    int lc = *line == ',';
    int ln = *line == '\n';
    int lz = *line == '\0';
    if (lc | ln | lz)
      return *line = '\0', line + (lc ? 2 : 1);
    line++;
  }
  return line;
}

static inline void get_gpu_info(struct gpu_record *gpu) {
  gpu->num_gpus = 0;

  // Try Asahi/M1 GPU first (native Linux driver)
  if (detect_asahi_gpu()) {
    get_asahi_gpu_info(gpu);
    return;
  }

  // Fall back to NVIDIA if present
  FILE *fp = popen(nvsmi_cmd, "r");
  if (!fp) {
    // nvidia-smi not available, no GPUs
    return;
  }

  // fread into a big static buffer, and null terminate.
  char nvsmi_contents[PAGE_SIZE * MAX_NUM_GPUS];
  size_t n_read = fread(nvsmi_contents, 1, sizeof(nvsmi_contents) - 1, fp);
  int status = pclose(fp);
  if (!n_read || status != 0) {
    // No output or nvidia-smi failed, no GPUs
    return;
  }
  nvsmi_contents[n_read] = '\0';

  char *line = nvsmi_contents;
  char *end = nvsmi_contents + n_read;
  for (size_t i = 0; i < MAX_NUM_GPUS; i++) {

    char *gpu_name = line;
    line = next_gpu_item(line, end);
    char *gpu_sm_utilization = line;
    line = next_gpu_item(line, end);
    char *gpu_mem_bandwidth_utilization = line;
    line = next_gpu_item(line, end);

    char *gpu_mem_total = line;
    line = next_gpu_item(line, end);
    char *gpu_mem_used = line;
    line = next_gpu_item(line, end);
    char *gpu_mem_free = line;
    line = next_gpu_item(line, end);

    char *gpu_graphics_clock = line;
    line = next_gpu_item(line, end);
    char *gpu_mem_clock = line;
    line = next_gpu_item(line, end);
    char *gpu_video_clock = line;
    line = next_gpu_item(line, end);

    char *gpu_power_draw = line;
    line = next_gpu_item(line, end);
    char *gpu_temp = line;
    line = next_gpu_item(line, end);

    int err = 0;
    // Use strncpy to prevent buffer overflow
    strncpy(gpu->gpu[i].gpu_name, gpu_name, sizeof(gpu->gpu[i].gpu_name) - 1);
    gpu->gpu[i].gpu_name[sizeof(gpu->gpu[i].gpu_name) - 1] = '\0';
    gpu->gpu[i].gpu_sm_utilization = str_to_u32(gpu_sm_utilization, &err);
    gpu->gpu[i].gpu_mem_bandwidth_utilization =
        str_to_u32(gpu_mem_bandwidth_utilization, &err);
    gpu->gpu[i].gpu_mem_total = str_to_u32(gpu_mem_total, &err);
    gpu->gpu[i].gpu_mem_used = str_to_u32(gpu_mem_used, &err);
    gpu->gpu[i].gpu_mem_free = str_to_u32(gpu_mem_free, &err);

    // Handle division by zero gracefully
    if (gpu->gpu[i].gpu_mem_total > 0) {
      gpu->gpu[i].gpu_mem_used_percentage =
          100.0 * ((float)gpu->gpu[i].gpu_mem_used / (float)gpu->gpu[i].gpu_mem_total);
    } else {
      gpu->gpu[i].gpu_mem_used_percentage = 0.0;
    }
    gpu->gpu[i].gpu_graphics_clock = str_to_u32(gpu_graphics_clock, &err);
    gpu->gpu[i].gpu_mem_clock = str_to_u32(gpu_mem_clock, &err);
    gpu->gpu[i].gpu_video_clock = str_to_u32(gpu_video_clock, &err);
    gpu->gpu[i].gpu_power_draw = str_to_u32(gpu_power_draw, &err);
    gpu->gpu[i].gpu_temp = str_to_u32(gpu_temp, &err);
    if (err)
      puts("Failed to parse nvidia-smi output."), exit(1);

    gpu->num_gpus++;
    if (!*line)
      break;
  }
}

static inline void get_cpu_info(cpu_record *cpu) {
  cpu->num_cpus = 0;

  // read all of /proc/stat into cpu_line.
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    puts("Failed to open /proc/stat."), exit(1);

  char stat_contents[81920];
  size_t n_read = fread(stat_contents, 1, sizeof(stat_contents) - 1, fp);
  if (!n_read)
    puts("Failed to read from /proc/stat."), exit(1);
  fclose(fp);
  stat_contents[n_read] = '\0';

  // Pass over the first line.
  char *p = stat_contents;
  char *end = stat_contents + n_read;
  while (*p && *p != '\n' && p < end)
    p++;
  if (p < end)
    p++;

  // Parse the cpuX fields.
  while (p < end) {

    // Verify that the line starts with "cpu".
    char *name = p;
    if (*p++ != 'c')
      return;
    if (*p++ != 'p')
      return;
    if (*p++ != 'u')
      return;

    // Check bounds before adding CPU
    if (cpu->num_cpus >= MAX_NUM_CPUS) {
      puts("Too many CPUs detected. Exiting."), exit(1);
    }

    // Parse the cpu number with bounds checking
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end)
      *p++ = '\0';
    strncpy(cpu->cpu[cpu->num_cpus].cpu_number, name, sizeof(cpu->cpu[cpu->num_cpus].cpu_number) - 1);
    cpu->cpu[cpu->num_cpus].cpu_number[sizeof(cpu->cpu[cpu->num_cpus].cpu_number) - 1] = '\0';

    // Parse the cpu fields with bounds checking
    int err = 0;

    char *user = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].user = str_to_u32(user, &err);

    char *nice = p; // Skip nice because it's not used in the calculation.
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) p++;
    (void)nice;

    char *system = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].system = str_to_u32(system, &err);

    char *idle = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].idle = str_to_u32(idle, &err);

    char *iowait = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].iowait = str_to_u32(iowait, &err);

    char *irq = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].irq = str_to_u32(irq, &err);

    char *softirq = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].softirq = str_to_u32(softirq, &err);

    char *steal = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].steal = str_to_u32(steal, &err);

    char *guest = p;
    while (*p && *p != ' ' && p < end)
      p++;
    if (p < end) *p++ = '\0';
    cpu->cpu[cpu->num_cpus].guest = str_to_u32(guest, &err);

    char *guest_nice =
        p; // Skip guest_nice, because it's not used in the calculation.
    while (*p && ((*p != ' ') & (*p != '\n')) && p < end)
      p++;
    if (p < end) p++;
    (void)guest_nice;

    if (err)
      puts("Failed to parse /proc/stat."), exit(1);

    cpu->num_cpus++;
  }
}

static inline void get_mem_info(mem_record *mem) {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp)
    puts("Failed to open /proc/meminfo."), exit(1);

  char meminfo_contents[16384];
  size_t n_read = fread(meminfo_contents, 1, sizeof(meminfo_contents) - 1, fp);
  if (!n_read)
    puts("Failed to read from /proc/meminfo."), exit(1);
  fclose(fp);
  meminfo_contents[n_read] = '\0';

  // Unrolled by gcc/clang
  struct {
    char *name;
    uint32_t *field;
    int found;
  } items[] = {
      {"MemTotal:", &mem->mem_total, 0},
      {"MemAvailable:", &mem->mem_free, 0},
      {"SwapTotal:", &mem->swp_total, 0},
      {"SwapFree:", &mem->swp_free, 0},
  };
  size_t n_items = sizeof(items) / sizeof(items[0]);
  size_t n_found = 0;

  char *p = meminfo_contents;
  char *end = meminfo_contents + n_read;
  while (1) {
    // Look for the items in order
    int hit_one = 0;
    if (!items[n_found].found && p < end) {
      p = read_memitem(p, end, items[n_found].name, items[n_found].field, &items[n_found].found);
      if (items[n_found].found) {
        hit_one = 1;
        n_found++;
      }
    }

    // No more items to find
    if (n_found == n_items)
      break;

    // Skip ahead to the next line if we didn't find anything
    if (!hit_one) {
      if (*p && p < end) {
        while (*p && *p != '\n' && p < end)
          p++;
        if (*p == '\n' && p < end)
          p++;
      } else {
        break;
      }
    }
  }

  // Calculate other fields
  mem->mem_used = mem->mem_total - mem->mem_free;
  mem->swp_used = mem->swp_total - mem->swp_free;

  // Handle division by zero gracefully (show 0% instead of NaN)
  if (mem->mem_total > 0) {
    mem->mem_percentage = 100.0 * ((float)mem->mem_used / (float)mem->mem_total);
  } else {
    mem->mem_percentage = 0.0;
  }

  if (mem->swp_total > 0) {
    mem->swp_percentage = 100.0 * ((float)mem->swp_used / (float)mem->swp_total);
  } else {
    mem->swp_percentage = 0.0;
  }
}

static inline float *calculate_cpu_utilization(cpu_record *prev,
                                               cpu_record *current) {
  size_t num_cpus = prev->num_cpus;
  if (num_cpus == 0)
    puts("No CPUs found. Exiting."), exit(1);
  if (prev->num_cpus != current->num_cpus)
    puts("Number of CPUs changed. Exiting."), exit(1);

  for (size_t i = 0; i < num_cpus; i++) {
    uint32_t prev_idle = prev->cpu[i].idle + prev->cpu[i].iowait;
    uint32_t current_idle = current->cpu[i].idle + current->cpu[i].iowait;

    uint32_t prev_non_idle = prev->cpu[i].user + prev->cpu[i].system +
                             prev->cpu[i].irq + prev->cpu[i].softirq +
                             prev->cpu[i].steal + prev->cpu[i].guest;
    uint32_t current_non_idle = current->cpu[i].user + current->cpu[i].system +
                                current->cpu[i].irq + current->cpu[i].softirq +
                                current->cpu[i].steal + current->cpu[i].guest;

    uint32_t prev_total = prev_idle + prev_non_idle;
    uint32_t current_total = current_idle + current_non_idle;

    // Check for underflow/wraparound - if current < prev, counters wrapped or bad data
    if (current_idle < prev_idle || current_total < prev_total) {
      utilization[i] = 0.0;
      continue;
    }

    uint32_t idle_diff = current_idle - prev_idle;
    uint32_t total_diff = current_total - prev_total;

    float denom = (float)total_diff;
    float ratio_idle = denom ? (float)idle_diff / denom : 0.0;
    float percent_active = +(1.0 - ratio_idle) * 100; // Always positive to avoid signed zero.
    utilization[i] = percent_active;
  }

  avg_utilization = 0;
  for (size_t i = 0; i < num_cpus; i++)
    avg_utilization += utilization[i];
  avg_utilization /= num_cpus;

  return utilization;
}

static inline void get_prev_cpu_info() {
  const size_t psm1 = PAGE_SIZE - 1;
  const size_t shm_size = (sizeof(cpu_record) + 1 + psm1) & ~psm1;

  // Open the shared memory file with secure permissions (user-only)
  int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
  if (fd == -1)
    perror("shm_open"), puts("Failed to shm_open()."), exit(1);

  // Setting the size zeros the memory if it hasn't already been mapped.
  if (ftruncate(fd, shm_size) == -1)
    puts("Failed to ftruncate the shared memory file."), exit(1);

  char *shm_contents =
      mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_contents == MAP_FAILED)
    puts("Failed to mmap the shared memory file."), exit(1);

  // Dispose of the file descriptor.
  close(fd);

  // Check if it's the first time this process has been run.
  // If it is, we need to take new measurments and pack it in, so that we have a
  // reference point.
  char *flagptr = shm_contents + sizeof(cpu_record);
  if (*flagptr == '\0') {
    *flagptr = 1;
    get_cpu_info((cpu_record *)shm_contents);
  }

  // Check if it's the first time this function has been run in the current
  // process. If it is, yoink the shm_contents buffer. Or copy and unmap.
  if (!prev_cpu_info) {
    prev_cpu_info = (cpu_record *)shm_contents;
  } else {
    memcpy(prev_cpu_info, shm_contents, sizeof(cpu_record));
    munmap(shm_contents, shm_size);
  }
}

static inline void save_cpu_shm(cpu_record *cpu) {
  memcpy((char *)prev_cpu_info, cpu, sizeof(cpu_record));
}

// Print results

#define BUF_SIZE (4096 * 20)
#define PRN(...) do { \
  size_t remaining = BUF_SIZE - buf_len; \
  if (remaining > 0) { \
    int written = snprintf(buf + buf_len, remaining, __VA_ARGS__); \
    if (written > 0) buf_len += (written < (int)remaining) ? written : remaining - 1; \
  } \
} while(0)

static inline size_t print_cpu_utilization(size_t num_cpus, char *buf,
                                           size_t buf_len, int genmon,
                                           int bar) {
  if (genmon) PRN("<big><b><span weight='bold'>");
  PRN("CPU Utilization:");
  if (genmon) PRN("</span></b></big>");
  PRN("\n");
  for (size_t i = 0; i < num_cpus; i++) {
    if (bar) {
      PRN("  CPU %2zu: %2.0f%%\n", i, utilization[i]);
    } else {
      PRN("  CPU %2zu: %2.0f%%\n", i, utilization[i]);
    }
  }
  PRN("\n");
  return buf_len;
}

static inline size_t print_gpu_info(gpu_record *gpu, char *buf, size_t buf_len,
                                    int genmon) {
  struct gpu_instance *g;
  if (gpu->num_gpus == 0) {
    return buf_len;
  } else if (gpu->num_gpus == 1) {
    g = &gpu->gpu[0];
    if (genmon) PRN("<big><b><span weight='bold'>");
    PRN("%s:", g->gpu_name);
    if (genmon) PRN("</span></b></big>");
    PRN("\n");

    PRN("  GPU SM Utilization: %" PRIu32 "%%\n", g->gpu_sm_utilization);
    PRN("  GPU Mem Bandwidth Utilization: %" PRIu32 "%%\n",
        g->gpu_mem_bandwidth_utilization);
    PRN("  GPU Mem Total: %" PRIu32 "\n", g->gpu_mem_total);
    PRN("  GPU Mem Used: %" PRIu32 "\n", g->gpu_mem_used);
    PRN("  GPU Mem Free: %" PRIu32 "\n", g->gpu_mem_free);
    PRN("  GPU Graphics Clock: %" PRIu32 "\n", g->gpu_graphics_clock);
    PRN("  GPU Mem Clock: %" PRIu32 "\n", g->gpu_mem_clock);
    PRN("  GPU Video Clock: %" PRIu32 "\n", g->gpu_video_clock);
    PRN("  GPU Power Draw: %" PRIu32 "\n", g->gpu_power_draw);
    PRN("  GPU Temp: %" PRIu32 "°\n", g->gpu_temp);
  } else {
    // Multiple GPUs
    for (size_t i = 0; i < gpu->num_gpus; i++) {
      g = &gpu->gpu[i];
      if (genmon) PRN("<big><b><span weight='bold'>");
      PRN("GPU %zu - %s:", i, g->gpu_name);
      if (genmon) PRN("</span></b></big>");
      PRN("\n");

      PRN("  GPU SM Utilization: %" PRIu32 "%%\n", g->gpu_sm_utilization);
      PRN("  GPU Mem Bandwidth Utilization: %" PRIu32 "%%\n", g->gpu_mem_bandwidth_utilization);
      PRN("  GPU Mem Total: %" PRIu32 "\n", g->gpu_mem_total);
      PRN("  GPU Mem Used: %" PRIu32 "\n", g->gpu_mem_used);
      PRN("  GPU Mem Free: %" PRIu32 "\n", g->gpu_mem_free);
      PRN("  GPU Graphics Clock: %" PRIu32 "\n", g->gpu_graphics_clock);
      PRN("  GPU Mem Clock: %" PRIu32 "\n", g->gpu_mem_clock);
      PRN("  GPU Video Clock: %" PRIu32 "\n", g->gpu_video_clock);
      PRN("  GPU Power Draw: %" PRIu32 "\n", g->gpu_power_draw);
      PRN("  GPU Temp: %" PRIu32 "°\n", g->gpu_temp);
      if (i < gpu->num_gpus - 1)
        PRN("\n");
    }
  }
  return buf_len;
}

static inline size_t print_cpu_mem_info(mem_record *mem, char *buf,
                                        size_t buf_len, int genmon) {
  if (genmon) PRN("<big><b><span weight='bold'>");
  PRN("CPU MEMORY: %.2f%%", mem->mem_percentage);
  if (genmon) PRN("</span></b></big>");
  PRN("\n");
  PRN("  Total: %" PRIu32 "\n", mem->mem_total);
  PRN("  Used: %" PRIu32 "\n", mem->mem_used);
  PRN("  Free: %" PRIu32 "\n", mem->mem_free);
  PRN("\n");
  return buf_len;
}

static inline size_t print_swap_mem_info(mem_record *mem, char *buf,
                                         size_t buf_len, int genmon) {
  if (genmon) PRN("<big><b><span weight='bold'>");
  PRN("Swap MEMORY: %.2f%%", mem->swp_percentage);
  if (genmon) PRN("</span></b></big>");
  PRN("\n");
  PRN("  Total: %" PRIu32 "\n", mem->swp_total);
  PRN("  Used: %" PRIu32 "\n", mem->swp_used);
  PRN("  Free: %" PRIu32 "\n", mem->swp_free);
  PRN("\n");
  return buf_len;
}

static inline size_t print_gpu_mem_info(gpu_record *gpu, char *buf,
                                        size_t buf_len, int genmon) {
  struct gpu_instance *g;
  if (gpu->num_gpus == 0)
    return buf_len;
  else if (gpu->num_gpus == 1) {
    g = &gpu->gpu[0];
    if (genmon) PRN("<big><b><span weight='bold'>");
    PRN("GPU MEMORY: %.2f%%", g->gpu_mem_used_percentage);
    if (genmon) PRN("</span></b></big>");
    PRN("\n");
    PRN("  Total: %" PRIu32 "\n", g->gpu_mem_total);
    PRN("  Used: %" PRIu32 "\n", g->gpu_mem_used);
    PRN("  Free: %" PRIu32 "\n", g->gpu_mem_free);
    PRN("\n");
  } else {
    // Multiple GPUs
    for (size_t i = 0; i < gpu->num_gpus; i++) {
      g = &gpu->gpu[i];
      if (genmon) PRN("<big><b><span weight='bold'>");
      PRN("GPU %zu MEMORY: %.2f%%", i, g->gpu_mem_used_percentage);
      if (genmon) PRN("</span></b></big>");
      PRN("\n");
      PRN("  Total: %" PRIu32 "\n", g->gpu_mem_total);
      PRN("  Used: %" PRIu32 "\n", g->gpu_mem_used);
      PRN("  Free: %" PRIu32 "\n", g->gpu_mem_free);
      if (i < gpu->num_gpus - 1)
        PRN("\n");
    }
    PRN("\n");
  }
  return buf_len;
}

static inline size_t print_panel_text(char *buf, size_t buf_len) {
  // ((CPU Utilization %, Mem %, Swap %), (GPU Utilization %, GPU Mem %))
  PRN("<txt>");
  if (info.gpu_info.num_gpus > 0) {
    PRN("((%5.2f%%, %5.2f%%, %5.2f%%) (%5.2f%%, %5.2f%%))", avg_utilization,
        info.mem_info.mem_percentage, info.mem_info.swp_percentage,
        (float)info.gpu_info.gpu[0].gpu_sm_utilization,
        info.gpu_info.gpu[0].gpu_mem_used_percentage);
  } else {
    PRN("((%5.2f%%, %5.2f%%, %5.2f%%))", avg_utilization,
        info.mem_info.mem_percentage, info.mem_info.swp_percentage);
  }
  PRN("</txt>\n");
  return buf_len;
}

static inline size_t print_click_text(char *buf, size_t buf_len, int img) {
  if (img) {
    // This is broken
    // https://gitlab.xfce.org/panel-plugins/xfce4-genmon-plugin/-/issues/30
    PRN("<click>xfce4-taskmanager</click>\n");
  } else {
    PRN("<txtclick>xfce4-taskmanager</txtclick>\n");
  }
  return buf_len;
}

static inline size_t print_tooltip_text(char *buf, size_t buf_len, int genmon) {
  PRN("<tool><tt>\n");
  buf_len =
      print_cpu_utilization(info.cpu_info.num_cpus, buf, buf_len, genmon, 1);
  buf_len = print_cpu_mem_info(&info.mem_info, buf, buf_len, genmon);
  buf_len = print_swap_mem_info(&info.mem_info, buf, buf_len, genmon);
  buf_len = print_gpu_mem_info(&info.gpu_info, buf, buf_len, genmon);
  buf_len = print_gpu_info(&info.gpu_info, buf, buf_len, genmon);
  PRN("</tt></tool>\n");
  return buf_len;
}

static inline size_t print_genmon(char *buf, size_t buf_len) {
  // Print in pango markup
  // (https://developer-old.gnome.org/pygtk/stable/pango-markup-language.html)
  buf_len = print_panel_text(buf, buf_len);
  buf_len = print_click_text(buf, buf_len, 0);
  buf_len = print_tooltip_text(buf, buf_len, 1);
  return buf_len;
}

static inline size_t print_svg_header(char *buf, size_t buf_len, size_t width,
                                      size_t height, int topdown) {
  char transform[256];
  *transform = '\0';
  if (!topdown)
    sprintf(transform, " transform='scale(1,-1) translate(0,-%zu)'", height);
  PRN("<svg width='%zu' height='%zu'%s><g>\n", width, height, transform);
  return buf_len;
}

static inline size_t print_svg_rects(char *buf, size_t buf_len) {

  size_t first_margin = 1;
  size_t margin_col_width = 4;
  size_t cols_printed = 0;
  size_t num_cpus = info.cpu_info.num_cpus;
  size_t num_gpus = info.gpu_info.num_gpus;

  // CPU utilization
  const char *cpu_colors[] = {CPU_COLORS};
  const size_t num_cpu_colors = sizeof(cpu_colors) / sizeof(cpu_colors[0]);
  for (size_t i = 0; i < num_cpus; i++) {
    PRN("<rect width='3' height='%zu%%' x='%zu' y='0' fill='%s' />\n",
        (size_t)utilization[i],
        (margin_col_width * cols_printed + first_margin),
        cpu_colors[i % num_cpu_colors]);
    cols_printed++;
  }

  // Memory usage
  PRN("<rect width='3' height='%zu%%' x='%zu' y='0' fill='%s' />\n",
      (size_t)info.mem_info.mem_percentage,
      (margin_col_width * cols_printed + first_margin), MEM_COLOR);
  cols_printed++;

  // Swap usage
  PRN("<rect width='3' height='%zu%%' x='%zu' y='0' fill='%s' />\n",
      (size_t)info.mem_info.swp_percentage,
      (margin_col_width * cols_printed + first_margin), SWP_COLOR);
  cols_printed++;

  // GPU utilization
  const char *gpu_colors[] = {GPU_COLORS};
  const size_t num_gpu_colors = sizeof(gpu_colors) / sizeof(gpu_colors[0]);
  for (size_t i = 0; i < num_gpus; i++) {
    PRN("<rect width='3' height='%zu%%' x='%zu' y='0' fill='%s' />\n",
        (size_t)info.gpu_info.gpu[i].gpu_sm_utilization,
        (margin_col_width * cols_printed + first_margin),
        gpu_colors[i % num_gpu_colors]);
    cols_printed++;
  }

  // VRAM usage
  for (size_t i = 0; i < num_gpus; i++) {
    PRN("<rect width='3' height='%zu%%' x='%zu' y='0' fill='%s' />\n",
        (size_t)info.gpu_info.gpu[i].gpu_mem_used_percentage,
        (margin_col_width * cols_printed + first_margin), VRAM_COLOR);
    cols_printed++;
  }

  return buf_len;
}

static inline size_t print_svg_footer(char *buf, size_t buf_len) {
  PRN("</g></svg>\n");
  return buf_len;
}

static inline void write_svg_file(char *buf, size_t buf_len, int topdown) {

  size_t width = 1; // start margin and 3px plus 1px margin for each rect
  width += info.cpu_info.num_cpus * 4; // cpu utilization
  width += 4;                          // mem
  width += 4;                          // swap
  width += info.gpu_info.num_gpus * 4; // gpu utilization
  width += info.gpu_info.num_gpus * 4; // vram

  size_t height = 28;

  buf_len = print_svg_header(buf, buf_len, width, height, topdown);
  buf_len = print_svg_rects(buf, buf_len);
  buf_len = print_svg_footer(buf, buf_len);

  // Use O_NOFOLLOW to prevent symlink attacks, 0644 for reasonable permissions
  int fd = open(tmp_svg, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0644);
  if (fd >= 0) {
    (void)!write(fd, buf, buf_len);
    close(fd);
  }
}

static inline size_t print_svg_img(char *buf, size_t buf_len) {
  PRN("<img>%s</img>\n", tmp_svg);
  return buf_len;
}

static inline size_t print_svg(char *buf, size_t buf_len, int topdown) {
  write_svg_file(buf, buf_len, topdown);
  buf_len = print_svg_img(buf, buf_len);
  buf_len = print_click_text(buf, buf_len, 1);
  buf_len = print_tooltip_text(buf, buf_len, 1);
  return buf_len;
}

static inline size_t print_bar(char *buf, size_t buf_len, size_t length,
                               float percent_full) {
  PRN("[");
  size_t bar_width = length * (percent_full / 100);
  for (size_t j = 0; j < length; j++) {
    if (j < bar_width) {
      PRN("#");
    } else {
      PRN(" ");
    }
  }
  PRN("]");
  return buf_len;
}

static inline size_t print_tui(char *buf, size_t buf_len) {
  PRN("\033[2J\033[H"); // Clear screen and move cursor to top-left corner
  PRN(ANSI_COLOR_CYAN "System Monitor\n" ANSI_COLOR_RESET);
  PRN("==========================================\n\n");

  // CPU Utilization
  PRN(ANSI_COLOR_BLUE "CPU Utilization: " ANSI_COLOR_RESET "%.2f%%\n",
      avg_utilization);
  if (info.cpu_info.num_cpus < 32) {
    for (size_t i = 0; i < info.cpu_info.num_cpus; i++) {
      PRN("  CPU %2zu: ", i);
      buf_len = print_bar(buf, buf_len, 50, utilization[i] / 2);
      PRN(" %.2f%%\n", utilization[i]);
    }
  }
  PRN("\n");

  // Memory Usage
  PRN(ANSI_COLOR_YELLOW "Memory Usage: " ANSI_COLOR_RESET "%.2f%%\n",
      info.mem_info.mem_percentage);
  PRN("  Total: %" PRIu32 " MB\n", info.mem_info.mem_total / 1024);
  PRN("  Used:  %" PRIu32 " MB\n", info.mem_info.mem_used / 1024);
  PRN("  Free:  %" PRIu32 " MB\n\n", info.mem_info.mem_free / 1024);

  // Swap Usage
  PRN(ANSI_COLOR_MAGENTA "Swap Usage: " ANSI_COLOR_RESET "%.2f%%\n",
      info.mem_info.swp_percentage);
  PRN("  Total: %" PRIu32 " MB\n", info.mem_info.swp_total / 1024);
  PRN("  Used:  %" PRIu32 " MB\n", info.mem_info.swp_used / 1024);
  PRN("  Free:  %" PRIu32 " MB\n\n", info.mem_info.swp_free / 1024);

  // GPU Information
  if (info.gpu_info.num_gpus == 1) {
    struct gpu_instance *g = &info.gpu_info.gpu[0];
    PRN(ANSI_COLOR_GREEN "GPU Information:" ANSI_COLOR_RESET "\n");
    PRN("  Name: %s\n", g->gpu_name);
    PRN("  SM Utilization:     %" PRIu32 "%%\n", g->gpu_sm_utilization);
    PRN("  Memory Usage:    %.2f%% (%f GiB / %u GiB)\n",
        g->gpu_mem_used_percentage, g->gpu_mem_used / 1024.,
        g->gpu_mem_total / 1024);
    PRN("  Temperature:     %" PRIu32 "°C\n", g->gpu_temp);
    PRN("  Power Draw:      %" PRIu32 " W\n", g->gpu_power_draw);
    PRN("\n");
  } else if (info.gpu_info.num_gpus > 1) {
    PRN(ANSI_COLOR_GREEN "GPU Information:" ANSI_COLOR_RESET "\n");
    for (size_t i = 0; i < info.gpu_info.num_gpus; i++) {
      struct gpu_instance *g = &info.gpu_info.gpu[i];
      PRN("  GPU %zu: %s\n", i, g->gpu_name);
      PRN("    SM Utilization:  %" PRIu32 "%%\n", g->gpu_sm_utilization);
      PRN("    Memory Usage:    %.2f%% (%.2f GiB / %.2f GiB)\n",
          g->gpu_mem_used_percentage, (float)g->gpu_mem_used / 1024.0,
          (float)g->gpu_mem_total / 1024.0);
      PRN("    Temperature:     %" PRIu32 "°C\n", g->gpu_temp);
      PRN("    Power Draw:      %" PRIu32 " W\n", g->gpu_power_draw);
      PRN("\n");
    }
  }
  return buf_len;
}

// M1 Chip Architecture Diagram - Apple-style big.LITTLE visualization
static inline size_t print_m1_chip_svg(char *buf, size_t buf_len) {
  // Panel height is 69px, design for that
  const size_t svg_height = 69;
  const size_t svg_width = 240;  // Wide enough for 4 cores per row + margins

  const size_t header_height = 10;  // M1 rainbow gradient header
  const size_t p_core_height = 30;  // Performance cores (larger)
  const size_t e_core_height = 20;  // Efficiency cores (smaller)
  const size_t margin = 2;

  const size_t core_width = 55;  // Width of each core block
  const size_t core_spacing = 60;  // Spacing between cores

  // Start SVG
  PRN("<svg width='%zu' height='%zu' viewBox='0 0 %zu %zu'>\n",
      svg_width, svg_height, svg_width, svg_height);

  // Background
  PRN("<rect width='%zu' height='%zu' fill='#000000'/>\n", svg_width, svg_height);

  // M1 Rainbow Gradient Header
  PRN("<defs>\n");
  PRN("  <linearGradient id='m1rainbow' x1='0%%' y1='0%%' x2='100%%' y2='0%%'>\n");
  PRN("    <stop offset='0%%' style='stop-color:#FF0000'/>\n");    // Red
  PRN("    <stop offset='17%%' style='stop-color:#FF7F00'/>\n");   // Orange
  PRN("    <stop offset='33%%' style='stop-color:#FFFF00'/>\n");   // Yellow
  PRN("    <stop offset='50%%' style='stop-color:#00FF00'/>\n");   // Green
  PRN("    <stop offset='67%%' style='stop-color:#0000FF'/>\n");   // Blue
  PRN("    <stop offset='83%%' style='stop-color:#4B0082'/>\n");   // Indigo
  PRN("    <stop offset='100%%' style='stop-color:#9400D3'/>\n");  // Violet
  PRN("  </linearGradient>\n");
  PRN("</defs>\n");

  // Rainbow header bar
  PRN("<rect x='0' y='0' width='%zu' height='%zu' fill='url(#m1rainbow)'/>\n",
      svg_width, header_height);

  // M1 text on header (white)
  PRN("<text x='%zu' y='%zu' font-family='Arial,sans-serif' font-size='8' font-weight='bold' fill='#FFFFFF' text-anchor='middle'>M1</text>\n",
      svg_width / 2, header_height - 2);

  size_t y_offset = header_height + margin;

  // Performance Cores (Top Row) - Cores 0-3 (Firestorm)
  for (size_t i = 0; i < 4 && i < info.cpu_info.num_cpus; i++) {
    size_t x = margin + (i * core_spacing);
    float util = utilization[i];

    // Core outline (dark gray)
    PRN("<rect x='%zu' y='%zu' width='%zu' height='%zu' fill='#1a1a1a' stroke='#404040' stroke-width='1'/>\n",
        x, y_offset, core_width, p_core_height);

    // Utilization fill (blue gradient based on usage)
    size_t fill_height = (p_core_height - 4) * util / 100.0;
    if (fill_height > 0) {
      PRN("<rect x='%zu' y='%zu' width='%zu' height='%zu' fill='#3498DB' opacity='%.2f'/>\n",
          x + 2, y_offset + p_core_height - 2 - fill_height,
          core_width - 4, fill_height,
          0.3 + (util / 100.0 * 0.7));  // Opacity 0.3-1.0 based on util
    }

    // Core internal details (simplified microarchitecture representation)
    PRN("<rect x='%zu' y='%zu' width='%zu' height='2' fill='#606060'/>\n",
        x + 10, y_offset + 8, core_width - 20);
    PRN("<rect x='%zu' y='%zu' width='%zu' height='2' fill='#606060'/>\n",
        x + 10, y_offset + 14, core_width - 20);
    PRN("<rect x='%zu' y='%zu' width='%zu' height='2' fill='#606060'/>\n",
        x + 10, y_offset + 20, core_width - 20);

    // Core label
    PRN("<text x='%zu' y='%zu' font-family='monospace' font-size='7' fill='#FFFFFF' text-anchor='middle'>P%zu</text>\n",
        x + core_width / 2, y_offset + p_core_height - 4, i);
  }

  y_offset += p_core_height + margin;

  // Efficiency Cores (Bottom Row) - Cores 4-7 (Icestorm)
  for (size_t i = 4; i < 8 && i < info.cpu_info.num_cpus; i++) {
    size_t x = margin + ((i - 4) * core_spacing);
    float util = utilization[i];

    // Core outline (dark gray, smaller)
    PRN("<rect x='%zu' y='%zu' width='%zu' height='%zu' fill='#1a1a1a' stroke='#404040' stroke-width='1'/>\n",
        x, y_offset, core_width, e_core_height);

    // Utilization fill (lighter blue for E-cores)
    size_t fill_height = (e_core_height - 4) * util / 100.0;
    if (fill_height > 0) {
      PRN("<rect x='%zu' y='%zu' width='%zu' height='%zu' fill='#5DADE2' opacity='%.2f'/>\n",
          x + 2, y_offset + e_core_height - 2 - fill_height,
          core_width - 4, fill_height,
          0.3 + (util / 100.0 * 0.7));
    }

    // Core internal details (simpler for E-cores)
    PRN("<rect x='%zu' y='%zu' width='%zu' height='2' fill='#505050'/>\n",
        x + 10, y_offset + 6, core_width - 20);
    PRN("<rect x='%zu' y='%zu' width='%zu' height='2' fill='#505050'/>\n",
        x + 10, y_offset + 12, core_width - 20);

    // Core label
    PRN("<text x='%zu' y='%zu' font-family='monospace' font-size='6' fill='#CCCCCC' text-anchor='middle'>E%zu</text>\n",
        x + core_width / 2, y_offset + e_core_height - 3, i - 4);
  }

  PRN("</svg>\n");
  return buf_len;
}

// Write M1 arch diagram to SVG file and return genmon output
static inline size_t print_m1_arch_mode(char *buf, size_t buf_len) {
  // Generate M1 chip SVG
  buf_len = print_m1_chip_svg(buf, buf_len);

  // Write to temp file
  int fd = open(tmp_svg, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0644);
  if (fd >= 0) {
    (void)!write(fd, buf, buf_len);
    close(fd);
  }

  // Reset buffer for genmon output
  buf_len = 0;

  // Output ONLY the image tag - no text, no (genmon), no XXX
  PRN("<img>%s</img>\n", tmp_svg);
  buf_len = print_click_text(buf, buf_len, 1);
  buf_len = print_tooltip_text(buf, buf_len, 1);

  return buf_len;
}

#define MODE_PRINT 0
#define MODE_SVG 1
#define MODE_TUI 2
#define MODE_M1_ARCH 3

typedef struct {
  int mode;
  int upsidedown;
} Args;

static inline Args argparse(int argc, char **argv) {
  Args args = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      puts("Usage: sys-genmon [-h,--help] "
           "[-s,--svg] [-u,--upsidedown] "
           "[-a,--arch-diagram] [-c,--clear-shm] [-t,--tui]"),
          exit(0);
    } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--svg")) {
      args.mode = MODE_SVG;
    } else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--arch-diagram")) {
      args.mode = MODE_M1_ARCH;
    } else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--upsidedown")) {
      args.upsidedown = 1;
    } else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--tui")) {
      args.mode = MODE_TUI;
    } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--clear-shm")) {
      if (shm_unlink(shm_name) && errno != ENOENT)
        puts("Failed to close the shared memory object."), exit(1);
      exit(0);
    } else {
      printf("Unknown argument: %s\n", argv[i]), exit(1);
    }
  }
  return args;
}

static inline void calculate_utilizations(void) {
  get_prev_cpu_info();
  get_gpu_info(&info.gpu_info);
  get_mem_info(&info.mem_info);
  get_cpu_info(&info.cpu_info);
  calculate_cpu_utilization(prev_cpu_info, &info.cpu_info);
  save_cpu_shm(&info.cpu_info);
}

int main(int argc, char **argv) {

  // Initialize secure paths before anything else
  init_secure_paths();

  Args args = argparse(argc, argv);

  char buf[BUF_SIZE];
  size_t buf_len = buf[0] = 0;
  switch (args.mode) {
  case MODE_PRINT: // Print genmon in (() ()) format
    calculate_utilizations();
    buf_len = print_genmon(buf, buf_len);
    (void)!write(STDOUT_FILENO, buf, buf_len);
    break;
  case MODE_SVG: // Print genmon in SVG format
    calculate_utilizations();
    buf_len = print_svg(buf, buf_len, args.upsidedown);
    (void)!write(STDOUT_FILENO, buf, buf_len);
    break;
  case MODE_TUI: // TUI mode, for display in terminal
    while (1) {
      calculate_utilizations();
      buf_len = print_tui(buf, buf_len);
      (void)!write(STDOUT_FILENO, buf, buf_len);
      buf_len = 0;
      sleep(1);
    }
    break;
  case MODE_M1_ARCH: // M1 chip architecture diagram for panel
    calculate_utilizations();
    buf_len = print_m1_arch_mode(buf, buf_len);
    (void)!write(STDOUT_FILENO, buf, buf_len);
    break;
  default:
    puts("Invalid mode."), exit(1);
  }

  return 0;
}
