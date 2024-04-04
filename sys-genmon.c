
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
#include <unistd.h>

#define MAX_NUM_CPUS 256
#define MAX_NUM_GPUS 8
#define PAGE_SIZE 4096

#define SVG_BAR_WIDTH 3,

#define CPU_COLORS "#3498DB", "#2471A3"
#define GPU_COLORS "#76B900", "#27AE60"
#define MEM_COLOR "#F1C40F"
#define SWP_COLOR "#8E44AD"
#define VRAM_COLOR "#BADC00"

static const char *tmp_svg = "/tmp/sys-genmon.svg";
static const char *shm_name = "/genmon_shmem";
static char *shm_contents = NULL;

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
    struct cpu_instance {
      char cpu_name[16];
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
    int lc = *line == ',';
    int ln = *line == '\n';
    if (lc | ln)
      return *line = '\0', line + (lc ? 2 : 1);
    line++;
  }
}

static inline void get_gpu_info(struct gpu_record *gpu) {
  FILE *fp = popen(nvsmi_cmd, "r");
  if (!fp)
    puts("Failed to open nvidia-smi"), exit(1);

  // Read the line into a big static buffer.
  char nvsmi_contents[PAGE_SIZE];
  if (!fgets(nvsmi_contents, sizeof(nvsmi_contents), fp))
    puts("Failed to read from nvidia-smi"), exit(1);

  pclose(fp);

  char *line = nvsmi_contents;
  for (size_t i = 0; i < MAX_NUM_GPUS; i++) {

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
    line = next_gpu_item(line);

    int err = 0;
    strcpy(gpu->gpu[i].gpu_name, gpu_name);
    gpu->gpu[i].gpu_sm_utilization = str_to_u32(gpu_sm_utilization, &err);
    gpu->gpu[i].gpu_mem_bandwidth_utilization =
        str_to_u32(gpu_mem_bandwidth_utilization, &err);
    gpu->gpu[i].gpu_mem_total = str_to_u32(gpu_mem_total, &err);
    gpu->gpu[i].gpu_mem_used = str_to_u32(gpu_mem_used, &err);
    gpu->gpu[i].gpu_mem_free = str_to_u32(gpu_mem_free, &err);
    gpu->gpu[i].gpu_mem_used_percentage =
        100.0 *
        ((float)gpu->gpu[i].gpu_mem_used / (float)gpu->gpu[i].gpu_mem_total);
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

    char *nice = p; // Skip nice because it's not used in the calculation.
    while (*p && *p != ' ')
      p++;
    p++;
    (void)nice;

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

    char *guest_nice =
        p; // Skip guest_nice, because it's not used in the calculation.
    while (*p && ((*p != ' ') & (*p != '\n')))
      p++;
    p++;
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

  typedef struct {
    char *name;
    uint32_t *field;
  } memitem;

  memitem items[] = {
      {"MemTotal:", &mem->mem_total},
      {"MemAvailable:", &mem->mem_free},
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

    utilization[i] = 100.0 * (1.0 - (float)(current_idle - prev_idle) /
                                        (float)(current_total - prev_total));
  }

  avg_utilization = 0;
  for (size_t i = 0; i < num_cpus; i++)
    avg_utilization += utilization[i];
  avg_utilization /= num_cpus;

  return utilization;
}

static inline cpu_record *get_prev_cpu_info(int persist) {

  if (!persist) {
    static cpu_record prev_cpu_info;
    get_cpu_info(&prev_cpu_info);
    return &prev_cpu_info;
  }

  const size_t psm1 = PAGE_SIZE - 1;
  const size_t shm_size = (sizeof(cpu_record) + 1 + psm1) & ~psm1;

  // Open the shared memory file.
  int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
  if (fd == -1)
    perror("shm_open"), puts("Failed to shm_open()."), exit(1);

  // Setting the size zeros the memory if it hasn't already been mapped.
  if (ftruncate(fd, shm_size) == -1)
    puts("Failed to ftruncate the shared memory file."), exit(1);

  shm_contents =
      mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_contents == MAP_FAILED)
    puts("Failed to mmap the shared memory file."), exit(1);

  // Check if it's the first run. If it is, we need to get new CPU info.
  char *flagptr = shm_contents + sizeof(cpu_record);
  if (*flagptr == '\0') {
    *flagptr = 1;
    get_cpu_info((cpu_record *)shm_contents);
  }
  return (cpu_record *)shm_contents;
}

static inline void save_cpu_shm(cpu_record *cpu, int persist) {
  if (persist)
    memcpy(shm_contents, cpu, sizeof(cpu_record));
}

// Print results

#define PRN(...) buf_len += sprintf(buf + buf_len, __VA_ARGS__)

static inline size_t print_cpu_utilization(size_t num_cpus, char *buf,
                                           size_t buf_len, int genmon) {
  if (genmon)
    PRN("<big><b><span weight='bold'>");
  PRN("CPU Utilization:");
  if (genmon)
    PRN("</span></b></big>");
  PRN("\n");
  for (size_t i = 0; i < num_cpus; i++)
    PRN("  CPU %zu: %.2f%%\n", i, utilization[i]);
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
    if (genmon)
      PRN("<big><b><span weight='bold'>");
    PRN("%s:", g->gpu_name);
    if (genmon)
      PRN("</span></b></big>");
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
    PRN("Multiple GPUs found. Printing multiple GPUs not yet implemented.\n");
    exit(1);
  }
  return buf_len;
}

static inline size_t print_cpu_mem_info(mem_record *mem, char *buf,
                                        size_t buf_len, int genmon) {
  if (genmon)
    PRN("<big><b><span weight='bold'>");
  PRN("CPU MEMORY: %.2f%%", mem->mem_percentage);
  if (genmon)
    PRN("</span></b></big>");
  PRN("\n");
  PRN("  Total: %" PRIu32 "\n", mem->mem_total);
  PRN("  Used: %" PRIu32 "\n", mem->mem_used);
  PRN("  Free: %" PRIu32 "\n", mem->mem_free);
  PRN("\n");
  return buf_len;
}

static inline size_t print_swap_mem_info(mem_record *mem, char *buf,
                                         size_t buf_len, int genmon) {
  if (genmon)
    PRN("<big><b><span weight='bold'>");
  PRN("Swap MEMORY: %.2f%%", mem->swp_percentage);
  if (genmon)
    PRN("</span></b></big>");
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
    if (genmon)
      PRN("<big><b><span weight='bold'>");
    PRN("GPU MEMORY: %.2f%%", g->gpu_mem_used_percentage);
    if (genmon)
      PRN("</span></b></big>");
    PRN("\n");
    PRN("  Total: %" PRIu32 "\n", g->gpu_mem_total);
    PRN("  Used: %" PRIu32 "\n", g->gpu_mem_used);
    PRN("  Free: %" PRIu32 "\n", g->gpu_mem_free);
    PRN("\n");
  } else {
    PRN("Multiple GPUs found. Printing multiple GPUs not yet implemented.\n");
    exit(1);
  }
  return buf_len;
}

static inline size_t print_panel_text(char *buf, size_t buf_len) {
  // ((CPU Utilization %, Mem %, Swap %), (GPU Utilization %, GPU Mem %))
  PRN("<txt>");
  PRN("((%5.2f%%, %5.2f%%, %5.2f%%) (%5.2f%%, %5.2f%%))", avg_utilization,
      info.mem_info.mem_percentage, info.mem_info.swp_percentage,
      (float)info.gpu_info.gpu[0].gpu_sm_utilization,
      info.gpu_info.gpu[0].gpu_mem_used_percentage);
  PRN("</txt>\n");
  return buf_len;
}

static inline size_t print_click_text(char *buf, size_t buf_len, int img) {
  if (img) {
    // This is broken
    // https://gitlab.xfce.org/panel-plugins/xfce4-genmon-plugin/-/issues/30
    // PRN("<click>xfce4-taskmanager</click>\n");
  } else {
    PRN("<txtclick>xfce4-taskmanager</txtclick>\n");
  }
  return buf_len;
}

static inline size_t print_tooltip_text(char *buf, size_t buf_len, int genmon) {
  PRN("<tool><tt>\n");
  buf_len = print_cpu_utilization(info.cpu_info.num_cpus, buf, buf_len, genmon);
  buf_len = print_cpu_mem_info(&info.mem_info, buf, buf_len, genmon);
  buf_len = print_swap_mem_info(&info.mem_info, buf, buf_len, genmon);
  buf_len = print_gpu_mem_info(&info.gpu_info, buf, buf_len, genmon);
  buf_len = print_gpu_info(&info.gpu_info, buf, buf_len, genmon);
  PRN("</tt></tool>\n");
  return buf_len;
}

static inline size_t print_css_text(char *buf, size_t buf_len) {
  return buf_len;
}

static inline size_t print_genmon(char *buf, size_t buf_len) {
  // Print in pango markup
  // (https://developer-old.gnome.org/pygtk/stable/pango-markup-language.html)
  buf_len = print_panel_text(buf, buf_len);
  buf_len = print_click_text(buf, buf_len, 0);
  buf_len = print_tooltip_text(buf, buf_len, 1);
  buf_len = print_css_text(buf, buf_len);
  return buf_len;
}

static inline size_t print_svg_header(char *buf, size_t buf_len, size_t width,
                                      size_t height, int topdown) {
  char transform[256];
  *transform = '\0';
  if (topdown)
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

  const char *cpu_colors[] = {CPU_COLORS};
  const size_t num_cpu_colors = sizeof(cpu_colors) / sizeof(cpu_colors[0]);

  const char *gpu_colors[] = {GPU_COLORS};
  const size_t num_gpu_colors = sizeof(gpu_colors) / sizeof(gpu_colors[0]);

  // CPU utilization
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

  size_t height = 30;

  buf_len = print_svg_header(buf, buf_len, width, height, topdown);
  buf_len = print_svg_rects(buf, buf_len);
  buf_len = print_svg_footer(buf, buf_len);

  int fd = open(tmp_svg, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  write(fd, buf, buf_len);
  close(fd);
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
  buf_len = print_css_text(buf, buf_len);
  return buf_len;
}

typedef struct {
  int persist;
  int svg;
  int topdown;
} Args;

static inline Args argparse(int argc, char **argv) {
  Args args = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      puts("Usage: sys-genmon [-h,--help] "
           "[-s,--svg] [-t,--topdown] "
           "[-p,--persist] [-c,--clear-shm]"),
          exit(0);
    } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--svg")) {
      args.svg = 1;
    } else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--topdown")) {
      args.topdown = 1;
    } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--persist")) {
      args.persist = 1;
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

int main(int argc, char **argv) {

  Args args = argparse(argc, argv);

  struct cpu_record *prev_cpu_info = get_prev_cpu_info(args.persist);
  get_gpu_info(&info.gpu_info);
  get_mem_info(&info.mem_info);
  get_cpu_info(&info.cpu_info);

  calculate_cpu_utilization(prev_cpu_info, &info.cpu_info);

  save_cpu_shm(&info.cpu_info, args.persist);

  char buf[4096 * 20];
  size_t buf_len = buf[0] = 0;
  if (args.svg)
    buf_len = print_svg(buf, buf_len, args.topdown);
  else
    buf_len = print_genmon(buf, buf_len);

  write(STDOUT_FILENO, buf, buf_len);

  return 0;
}