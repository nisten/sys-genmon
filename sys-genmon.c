
// Requires linux 2.6.33 (Released Feb 2010) or later.
// Assumes that the number of CPUs doesn't change while the program is running.

// Statistics that I care about:
// --------------------------------
// 1. CPU usage - all cores - Blue, alternating
// 2. GPU utilization % - Green
// --------------------------------
// 3. Memory usage - Yellow
// 4. Swap usage - Purple
// 5. Vram usage -
// --------------------------------
// 6. CPU temp - Red
// 7. GPU temp - Orange
// --------------------------------
// 8. Disk usage
// 9. Network usage.
// --------------------------------

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_NUM_CPUS 256

static const char *shmem = "/tmp/genmon_shmem";

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

struct all_info {
  struct cpu_record {
    struct {
      char cpu_name[16];
      uint32_t user;
      uint32_t nice;
      uint32_t system;

      uint32_t idle;
      uint32_t iowait;
      uint32_t irq;
      uint32_t softirq;

      uint32_t steal;
      uint32_t guest;
      uint32_t guest_nice;
    } cpu[MAX_NUM_CPUS];
    size_t num_cpus;
  } cpu_info;

  struct gpu_record {
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
} info_prev, info_current;

uint32_t utilizations[MAX_NUM_CPUS];

typedef struct cpu_record cpu_record;
typedef struct gpu_record gpu_record;
typedef struct mem_record mem_record;

static inline uint32_t str_to_u32(char *s, int *err) {
#if UINT32_MAX <= ULONG_MAX
#define strtot unsigned long
#define strto strtoul
#elif UINT32_MAX <= ULLONG_MAX
#define strtot unsigned long long
#define strto strtoull
#endif

  errno = 0;
  strtot ul = strto(s, NULL, 10);
  if (errno | (ul > UINT32_MAX))
    *err = 1;
  return ul;
}

static inline int starts_with(char *s, char *start) {
  while (*start & *s) {
    if (*start != *s)
      return 0;
    start++;
    s++;
  }
  return 1;
}

static inline char *read_memitem(char *p, char *title, uint32_t *item) {
  if (!*p) {
    puts("Failed to parse /proc/meminfo. Ran out of input."), exit(1);
  } else if (starts_with(p, title)) {
    p += strlen(title);
    while (*p == ' ')
      p++;
    char *itemstr = p;
    while (*p != ' ')
      p++;
    if (*p != '\n')
      *p++ = '\0';
    while (*p != '\n')
      p++;
    *p++ = '\0';

    int err = 0;
    *item = str_to_u32(itemstr, &err);
    if (err)
      puts("Failed to parse /proc/meminfo. Invalid number format."), exit(1);
  }
  return p;
}

static inline char *next_gpu_item(char *line) {
  while (1) {
    if ((*line == ',') | (*line == '\n'))
      return *line = '\0', line + 2;
    line++;
  }
}

static inline void get_gpu_info(struct gpu_record *gpu) {
  FILE *fp = popen(nvsmi_cmd, "r");
  if (!fp)
    puts("Failed to open nvidia-smi"), exit(1);

  char nvsmi_contents[4096];

  // Read the line into a big static buffer.
  if (!fgets(nvsmi_contents, sizeof(nvsmi_contents), fp)) {
    puts("Failed to read from nvidia-smi"), exit(1);
  }

  pclose(fp);

  char *line = nvsmi_contents;
  char *gpu_name = line;
  line = next_gpu_item(line);
  char *gpu_sm_utilization = line;
  line = next_gpu_item(line);
  char *gpu_mem_bandwidth_utilization = line;
  line = next_gpu_item(line);

  char *gpu_mem_total = line;
  line = next_gpu_item(line);
  char *gpu_mem_used = line;
  line = next_gpu_item(line);
  char *gpu_mem_free = line;
  line = next_gpu_item(line);

  char *gpu_graphics_clock = line;
  line = next_gpu_item(line);
  char *gpu_mem_clock = line;
  line = next_gpu_item(line);
  char *gpu_video_clock = line;
  line = next_gpu_item(line);

  char *gpu_power_draw = line;
  line = next_gpu_item(line);
  char *gpu_temp = line;
  next_gpu_item(line);

  int err = 0;
  strcpy(gpu->gpu_name, gpu_name);
  gpu->gpu_sm_utilization = str_to_u32(gpu_sm_utilization, &err);
  gpu->gpu_mem_bandwidth_utilization =
      str_to_u32(gpu_mem_bandwidth_utilization, &err);
  gpu->gpu_mem_total = str_to_u32(gpu_mem_total, &err);
  gpu->gpu_mem_used = str_to_u32(gpu_mem_used, &err);
  gpu->gpu_mem_free = str_to_u32(gpu_mem_free, &err);
  gpu->gpu_mem_used_percentage =
      100.0 * ((float)gpu->gpu_mem_used / (float)gpu->gpu_mem_total);
  gpu->gpu_graphics_clock = str_to_u32(gpu_graphics_clock, &err);
  gpu->gpu_mem_clock = str_to_u32(gpu_mem_clock, &err);
  gpu->gpu_video_clock = str_to_u32(gpu_video_clock, &err);
  gpu->gpu_power_draw = str_to_u32(gpu_power_draw, &err);
  gpu->gpu_temp = str_to_u32(gpu_temp, &err);
  if (err)
    puts("Failed to parse nvidia-smi output."), exit(1);
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
  while (*p && *p != '\n')
    p++;
  p++;

  // Parse the cpuX fields.
  while (1) {

    // Verify that the line starts with "cpu".
    char *name = p;
    if (*p++ != 'c')
      return;
    if (*p++ != 'p')
      return;
    if (*p++ != 'u')
      return;

    // Parse the cpu number
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    strcpy(cpu->cpu[cpu->num_cpus].cpu_name, name);

    // Parse the cpu fields.
    int err = 0;

    char *user = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].user = str_to_u32(user, &err);

    char *nice = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].nice = str_to_u32(nice, &err);

    char *system = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].system = str_to_u32(system, &err);

    char *idle = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].idle = str_to_u32(idle, &err);

    char *iowait = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].iowait = str_to_u32(iowait, &err);

    char *irq = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].irq = str_to_u32(irq, &err);

    char *softirq = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].softirq = str_to_u32(softirq, &err);

    char *steal = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].steal = str_to_u32(steal, &err);

    char *guest = p;
    while (*p && *p != ' ')
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].guest = str_to_u32(guest, &err);

    char *guest_nice = p;
    while (*p && ((*p != ' ') & (*p != '\n')))
      p++;
    *p++ = '\0';
    cpu->cpu[cpu->num_cpus].guest_nice = str_to_u32(guest_nice, &err);

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

  typedef struct {
    char *name;
    uint32_t *field;
  } memitem;

  memitem items[] = {
      {"MemTotal:", &mem->mem_total},
      {"MemFree:", &mem->mem_free},
      {"SwapTotal:", &mem->swp_total},
      {"SwapFree:", &mem->swp_free},
  };
  size_t n_items = sizeof(items) / sizeof(items[0]);
  size_t n_found = 0;

  char *p = meminfo_contents;
  while (1) {
    int hit_one = 0;
    for (size_t i = 0; i < n_items; i++) {
      if (*items[i].field)
        continue;
      p = read_memitem(p, items[i].name, items[i].field);
      if (*items[i].field) {
        hit_one = 1;
        n_found++;
      }
    }

    // No more items to find
    if (n_found == n_items)
      break;

    if (!hit_one) {
      // Skip ahead to the next line if there is one, otherwise stop reading.
      if (*p) {
        while (*p && *p != '\n')
          p++;
        p++;
      } else {
        break;
      }
    }
  }

  // Calculate other fields
  mem->mem_used = mem->mem_total - mem->mem_free;
  mem->swp_used = mem->swp_total - mem->swp_free;
  mem->mem_percentage = 100.0 * ((float)mem->mem_used / (float)mem->mem_total);
  mem->swp_percentage = 100.0 * ((float)mem->swp_used / (float)mem->swp_total);
}

static inline int calculate_cpu_utilization(cpu_record *prev,
                                            cpu_record *current) {
  uint32_t total_time = 0;
}

#define PRN(...) buf_len += sprintf(buf + buf_len, __VA_ARGS__)

static inline size_t print_cpu_info(cpu_record *cpu, char *buf) {
  size_t buf_len = 0;
  for (size_t i = 0; i < cpu->num_cpus; i++) {
    PRN("%s:\n", cpu->cpu[i].cpu_name);
    PRN("  User: %" PRIu32 "\n", cpu->cpu[i].user);
    PRN("  Nice: %" PRIu32 "\n", cpu->cpu[i].nice);
    PRN("  System: %" PRIu32 "\n", cpu->cpu[i].system);
    PRN("  Idle: %" PRIu32 "\n", cpu->cpu[i].idle);
    PRN("  IOWait: %" PRIu32 "\n", cpu->cpu[i].iowait);
    PRN("  IRQ: %" PRIu32 "\n", cpu->cpu[i].irq);
    PRN("  SoftIRQ: %" PRIu32 "\n", cpu->cpu[i].softirq);
    PRN("  Steal: %" PRIu32 "\n", cpu->cpu[i].steal);
    PRN("  Guest: %" PRIu32 "\n", cpu->cpu[i].guest);
    PRN("  Guest Nice: %" PRIu32 "\n", cpu->cpu[i].guest_nice);
    PRN("\n");
  }
  return buf_len;
}

static inline size_t print_gpu_info(gpu_record *gpu, char *buf) {
  size_t buf_len = 0;
  PRN("%s:\n", gpu->gpu_name);
  PRN("  GPU SM Utilization: %" PRIu32 "%%\n", gpu->gpu_sm_utilization);
  PRN("  GPU Mem Bandwidth Utilization: %" PRIu32 "%%\n",
      gpu->gpu_mem_bandwidth_utilization);
  PRN("  GPU Mem Total: %" PRIu32 "\n", gpu->gpu_mem_total);
  PRN("  GPU Mem Used: %" PRIu32 "\n", gpu->gpu_mem_used);
  PRN("  GPU Mem Free: %" PRIu32 "\n", gpu->gpu_mem_free);
  PRN("  GPU Graphics Clock: %" PRIu32 "\n", gpu->gpu_graphics_clock);
  PRN("  GPU Mem Clock: %" PRIu32 "\n", gpu->gpu_mem_clock);
  PRN("  GPU Video Clock: %" PRIu32 "\n", gpu->gpu_video_clock);
  PRN("  GPU Power Draw: %" PRIu32 "\n", gpu->gpu_power_draw);
  PRN("  GPU Temp: %" PRIu32 "°\n", gpu->gpu_temp);
  PRN("\n");
  return buf_len;
}

static inline size_t print_cpu_mem_info(mem_record *mem, char *buf) {
  size_t buf_len = 0;
  PRN("CPU MEMORY: %.2f%%\n", mem->mem_percentage);
  PRN("  Total: %" PRIu32 "\n", mem->mem_total);
  PRN("  Used: %" PRIu32 "\n", mem->mem_used);
  PRN("  Free: %" PRIu32 "\n", mem->mem_free);
  PRN("\n");

  PRN("Swap MEMORY: %.2f%%\n", mem->swp_percentage);
  PRN("  Total: %" PRIu32 "\n", mem->swp_total);
  PRN("  Used: %" PRIu32 "\n", mem->swp_used);
  PRN("  Free: %" PRIu32 "\n", mem->swp_free);
  PRN("\n");
  return buf_len;
}

static inline size_t print_gpu_mem_info(gpu_record *gpu, char *buf) {
  size_t buf_len = 0;
  PRN("GPU MEMORY: %.2f%%\n", gpu->gpu_mem_used_percentage);
  PRN("  Total: %" PRIu32 "\n", gpu->gpu_mem_total);
  PRN("  Used: %" PRIu32 "\n", gpu->gpu_mem_used);
  PRN("  Free: %" PRIu32 "\n", gpu->gpu_mem_free);
  PRN("\n");
  return buf_len;
}

static inline void print_genmon(void) {
  char buf[4096 * 20];
  size_t buf_len = buf[0] = 0;

  buf_len += print_cpu_info(&info_current.cpu_info, buf + buf_len);
  buf_len += print_cpu_mem_info(&info_current.mem_info, buf + buf_len);
  buf_len += print_gpu_mem_info(&info_current.gpu_info, buf + buf_len);
  buf_len += print_gpu_info(&info_current.gpu_info, buf + buf_len);
  printf("%s", buf);
}

typedef struct {
  int persist;
  int genmon;
} Args;

static inline Args argparse(int argc, char **argv) {
  Args args = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-p"))
      args.persist = 1;
    else if (!strcmp(argv[i], "-g"))
      args.genmon = 1;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
      puts("Usage: sys-genmon [-p] [-g]"), exit(0);
    else
      printf("Unknown argument: %s\n", argv[i]), exit(1);
  }
  return args;
}

int main(int argc, char **argv) {

  Args args = argparse(argc, argv);

  if (!args.persist) {
    get_cpu_info(&info_prev.cpu_info);
    get_gpu_info(&info_current.gpu_info);
    get_cpu_info(&info_current.cpu_info);
    get_mem_info(&info_current.mem_info);
    calculate_cpu_utilization(&info_prev.cpu_info, &info_current.cpu_info);
  } else {
    puts("Not implemented.");
    return 1;
  }

  if (args.genmon) {
    print_genmon();
  } else {
    puts("Not implemented.");
    return 1;
  }

  return 0;
}