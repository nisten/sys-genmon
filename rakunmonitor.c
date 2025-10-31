/*
 * Raccoon Monitor - XFCE Panel Plugin for Apple M1 CPU Monitoring
 * Shows M1 chip architecture with real-time per-core utilization
 *
 * Copyright (c) 2025 - Built with love by Claude Code
 */

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <cairo.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_NUM_CPUS 256

/* Plugin structure */
typedef struct {
    XfcePanelPlugin *plugin;

    /* Widgets */
    GtkWidget *ebox;
    GtkWidget *image;

    /* Update timer */
    guint timeout_id;

    /* CPU data */
    struct cpu_instance {
        char cpu_number[16];
        uint32_t user, system, idle, iowait, irq, softirq, steal, guest;
    } cpu_current[MAX_NUM_CPUS];
    struct cpu_instance cpu_prev[MAX_NUM_CPUS];
    size_t num_cpus;
    float utilization[MAX_NUM_CPUS];

    /* Shared memory for persistent stats */
    char shm_name[256];
    void *shm_ptr;
    size_t shm_size;
} RakunMonitor;

/* Parse /proc/stat to get CPU info */
static void get_cpu_info(RakunMonitor *rakun) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[512];
    rakun->num_cpus = 0;

    // Skip first summary line
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return;
    }

    // Parse individual CPU lines
    while (fgets(line, sizeof(line), fp) && rakun->num_cpus < MAX_NUM_CPUS) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (!isdigit(line[3])) continue;

        struct cpu_instance *cpu = &rakun->cpu_current[rakun->num_cpus];

        unsigned long nice, guest_nice;
        if (sscanf(line, "%15s %u %lu %u %u %u %u %u %u %u %lu",
                   cpu->cpu_number,
                   &cpu->user, &nice, &cpu->system, &cpu->idle,
                   &cpu->iowait, &cpu->irq, &cpu->softirq,
                   &cpu->steal, &cpu->guest, &guest_nice) >= 5) {
            rakun->num_cpus++;
        }
    }

    fclose(fp);
}

/* Calculate CPU utilization percentages */
static void calculate_utilization(RakunMonitor *rakun) {
    for (size_t i = 0; i < rakun->num_cpus; i++) {
        struct cpu_instance *prev = &rakun->cpu_prev[i];
        struct cpu_instance *curr = &rakun->cpu_current[i];

        uint32_t prev_idle = prev->idle + prev->iowait;
        uint32_t curr_idle = curr->idle + curr->iowait;

        uint32_t prev_non_idle = prev->user + prev->system + prev->irq +
                                  prev->softirq + prev->steal + prev->guest;
        uint32_t curr_non_idle = curr->user + curr->system + curr->irq +
                                  curr->softirq + curr->steal + curr->guest;

        uint32_t prev_total = prev_idle + prev_non_idle;
        uint32_t curr_total = curr_idle + curr_non_idle;

        // Check for underflow
        if (curr_idle < prev_idle || curr_total < prev_total) {
            rakun->utilization[i] = 0.0;
            continue;
        }

        uint32_t idle_diff = curr_idle - prev_idle;
        uint32_t total_diff = curr_total - prev_total;

        if (total_diff > 0) {
            float ratio_idle = (float)idle_diff / (float)total_diff;
            rakun->utilization[i] = (1.0 - ratio_idle) * 100.0;
        } else {
            rakun->utilization[i] = 0.0;
        }
    }
}

/* Render M1 chip architecture diagram to Cairo surface */
static void render_m1_chip(cairo_t *cr, RakunMonitor *rakun, int width, int height) {
    const int header_height = 10;
    const int p_core_height = 50;  // Performance cores - TWICE as tall!
    const int e_core_height = 26;  // Efficiency cores - 30% taller
    const int margin = 2;
    const int core_width = 66;   // Wider cores
    const int core_spacing = 73;  // More spacing to fill 290px canvas

    // Background (semi-transparent)
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    // Calculate average CPU utilization for dynamic rainbow
    float avg_util = 0.0;
    for (size_t i = 0; i < rakun->num_cpus; i++) {
        avg_util += rakun->utilization[i];
    }
    avg_util /= rakun->num_cpus;

    // Heat factor: 0.0 = cool (blue), 1.0 = hot (red)
    float heat = avg_util / 100.0;

    // M1 Dynamic Rainbow Gradient Header (shifts red when hot)
    cairo_pattern_t *rainbow = cairo_pattern_create_linear(0, 0, width, 0);

    // Shift color spectrum based on CPU load
    // High load: more red/orange/yellow
    // Low load: more blue/indigo/violet
    float red_boost = heat * 0.3;
    float blue_reduce = heat * 0.5;

    cairo_pattern_add_color_stop_rgb(rainbow, 0.00, 1.0, 0.0, 0.0);  // Red (always strong)
    cairo_pattern_add_color_stop_rgb(rainbow, 0.15 - (heat * 0.05), 1.0, 0.5 + red_boost, 0.0);  // Orange (shift left when hot)
    cairo_pattern_add_color_stop_rgb(rainbow, 0.30 - (heat * 0.1), 1.0, 1.0, 0.0);  // Yellow (shift left when hot)
    cairo_pattern_add_color_stop_rgb(rainbow, 0.50, 0.0, 1.0 - (heat * 0.3), 0.0);  // Green (dim when hot)
    cairo_pattern_add_color_stop_rgb(rainbow, 0.67, 0.0, 0.0, 1.0 - blue_reduce);  // Blue (dim when hot)
    cairo_pattern_add_color_stop_rgb(rainbow, 0.83, 0.29, 0.0, 0.51 - blue_reduce); // Indigo (dim when hot)
    cairo_pattern_add_color_stop_rgb(rainbow, 1.00, 0.58, 0.0, 0.83 - blue_reduce); // Violet (dim when hot)

    cairo_set_source(cr, rainbow);
    cairo_rectangle(cr, 0, 0, width, header_height);
    cairo_fill(cr);
    cairo_pattern_destroy(rainbow);

    // M1 text
    cairo_select_font_face(cr, "Arial", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 8);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, width/2 - 8, header_height - 2);
    cairo_show_text(cr, "M1");

    int y_offset = header_height + margin;

    // Performance Cores (Top Row) - Cores 0-3
    for (int i = 0; i < 4 && i < (int)rakun->num_cpus; i++) {
        int x = margin + (i * core_spacing);
        float util = rakun->utilization[i];

        // Core outline (transparent background)
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x, y_offset, core_width, p_core_height);
        cairo_stroke(cr);

        // Utilization fill (blue)
        if (util > 0) {
            int fill_height = (int)((p_core_height - 4) * util / 100.0);
            double alpha = 0.3 + (util / 100.0 * 0.7);
            cairo_set_source_rgba(cr, 0.2, 0.6, 0.86, alpha);
            cairo_rectangle(cr, x + 2, y_offset + p_core_height - 2 - fill_height,
                          core_width - 4, fill_height);
            cairo_fill(cr);
        }

        // Vertical lines (Apple M1 P-core style - 5 lines with notches)
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        cairo_set_line_width(cr, 2);
        const int notch_size = 4;
        for (int line = 0; line < 5; line++) {
            int line_x = x + 8 + (line * 12);

            if (line == 0 || line == 4) {
                // First and last lines: full height
                cairo_move_to(cr, line_x, y_offset + 4);
                cairo_line_to(cr, line_x, y_offset + p_core_height - 4);
                cairo_stroke(cr);
            } else {
                // Middle lines (1-3): have top and bottom notches
                cairo_move_to(cr, line_x, y_offset + 4 + notch_size);
                cairo_line_to(cr, line_x, y_offset + p_core_height - 4 - notch_size);
                cairo_stroke(cr);

                // Draw the notch marks (horizontal bars at top and bottom)
                cairo_move_to(cr, line_x - 3, y_offset + 4);
                cairo_line_to(cr, line_x + 3, y_offset + 4);
                cairo_stroke(cr);

                cairo_move_to(cr, line_x - 3, y_offset + p_core_height - 4);
                cairo_line_to(cr, line_x + 3, y_offset + p_core_height - 4);
                cairo_stroke(cr);
            }
        }
    }

    y_offset += p_core_height + margin;

    // Efficiency Cores (Bottom Row) - Cores 4-7
    for (int i = 4; i < 8 && i < (int)rakun->num_cpus; i++) {
        int x = margin + ((i - 4) * core_spacing);
        float util = rakun->utilization[i];

        // Core outline (transparent background)
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x, y_offset, core_width, e_core_height);
        cairo_stroke(cr);

        // Utilization fill (lighter blue)
        if (util > 0) {
            int fill_height = (int)((e_core_height - 4) * util / 100.0);
            double alpha = 0.3 + (util / 100.0 * 0.7);
            cairo_set_source_rgba(cr, 0.36, 0.68, 0.88, alpha);
            cairo_rectangle(cr, x + 2, y_offset + e_core_height - 2 - fill_height,
                          core_width - 4, fill_height);
            cairo_fill(cr);
        }

        // Horizontal lines (Apple M1 E-core style - 3 lines)
        cairo_set_source_rgb(cr, 0.32, 0.32, 0.32);
        cairo_set_line_width(cr, 2);
        for (int line = 0; line < 3; line++) {
            int line_y = y_offset + 6 + (line * 7);
            cairo_move_to(cr, x + 4, line_y);
            cairo_line_to(cr, x + core_width - 4, line_y);
            cairo_stroke(cr);
        }
    }
}

/* Update the display */
static gboolean rakun_update(gpointer user_data) {
    RakunMonitor *rakun = (RakunMonitor *)user_data;

    // Save previous CPU stats
    memcpy(rakun->cpu_prev, rakun->cpu_current, sizeof(rakun->cpu_current));

    // Get new CPU stats
    get_cpu_info(rakun);

    // Calculate utilization
    calculate_utilization(rakun);

    // Render to pixbuf
    const int img_width = 290;  // Another 10% wider (was 264)
    const int img_height = 92;  // 10 header + 50 P-cores + 2 margin + 26 E-cores + 2 margin + 2 padding

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_width, img_height);
    cairo_t *cr = cairo_create(surface);

    render_m1_chip(cr, rakun, img_width, img_height);

    // Convert Cairo surface to GdkPixbuf
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, img_width, img_height);

    // Update image widget
    gtk_image_set_from_pixbuf(GTK_IMAGE(rakun->image), pixbuf);

    // Cleanup
    g_object_unref(pixbuf);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return TRUE; // Continue timer
}

/* Plugin constructor */
static RakunMonitor *rakun_construct(XfcePanelPlugin *plugin) {
    RakunMonitor *rakun = g_slice_new0(RakunMonitor);

    rakun->plugin = plugin;

    // Initialize shared memory path
    snprintf(rakun->shm_name, sizeof(rakun->shm_name), "/rakunmon_shmem_%d", getuid());

    // Create event box (for tooltips/clicks)
    rakun->ebox = gtk_event_box_new();
    gtk_widget_show(rakun->ebox);

    // Create image widget
    rakun->image = gtk_image_new();
    gtk_widget_show(rakun->image);
    gtk_container_add(GTK_CONTAINER(rakun->ebox), rakun->image);

    // Add to panel
    gtk_container_add(GTK_CONTAINER(plugin), rakun->ebox);
    xfce_panel_plugin_add_action_widget(plugin, rakun->ebox);

    // Set tooltip
    gtk_widget_set_tooltip_text(rakun->ebox, "Raccoon Monitor - M1 CPU Architecture");

    // Get initial CPU stats (baseline)
    get_cpu_info(rakun);
    memcpy(rakun->cpu_prev, rakun->cpu_current, sizeof(rakun->cpu_current));

    // Initialize all utilization to 0 for first display
    for (size_t i = 0; i < MAX_NUM_CPUS; i++) {
        rakun->utilization[i] = 0.0;
    }

    // Render initial display with 0% utilization
    const int img_width = 290;
    const int img_height = 92;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_width, img_height);
    cairo_t *cr = cairo_create(surface);
    render_m1_chip(cr, rakun, img_width, img_height);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, img_width, img_height);
    gtk_image_set_from_pixbuf(GTK_IMAGE(rakun->image), pixbuf);
    g_object_unref(pixbuf);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    // Start update timer (2 second interval) - first update will have real data
    rakun->timeout_id = g_timeout_add(2000, rakun_update, rakun);

    return rakun;
}

/* Plugin destructor */
static void rakun_free(XfcePanelPlugin *plugin, RakunMonitor *rakun) {
    // Stop timer
    if (rakun->timeout_id != 0) {
        g_source_remove(rakun->timeout_id);
        rakun->timeout_id = 0;
    }

    // Free shared memory
    if (rakun->shm_ptr) {
        munmap(rakun->shm_ptr, rakun->shm_size);
        shm_unlink(rakun->shm_name);
    }

    // Free widgets
    gtk_widget_destroy(rakun->ebox);

    // Free structure
    g_slice_free(RakunMonitor, rakun);
}

/* Panel size changed callback */
static gboolean rakun_size_changed(XfcePanelPlugin *plugin, gint size, RakunMonitor *rakun) {
    // Force update with new size
    rakun_update(rakun);
    return TRUE;
}

/* Plugin registration */
static void rakun_construct_wrapper(XfcePanelPlugin *plugin) {
    RakunMonitor *rakun = rakun_construct(plugin);

    g_signal_connect(G_OBJECT(plugin), "free-data",
                     G_CALLBACK(rakun_free), rakun);

    g_signal_connect(G_OBJECT(plugin), "size-changed",
                     G_CALLBACK(rakun_size_changed), rakun);
}

/* Plugin registration macro */
XFCE_PANEL_PLUGIN_REGISTER(rakun_construct_wrapper);
