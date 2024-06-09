#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/tick.h>
#include <linux/cpumask.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel_stat.h>
#include <linux/times.h>
#include <linux/cpu.h>

#define MONITOR_INTERVAL 1 // Intervalo de monitoramento em segundos

static struct timer_list monitor_timer;
static struct proc_dir_entry *proc_entry;
static char *stats_buffer;
static size_t stats_buffer_size = 8192;
static DEFINE_MUTEX(stats_buffer_mutex);

u64 prev_user_time = 0;
u64 prev_system_time = 0;
u64 prev_softirq_time = 0;
u64 prev_irq_time = 0;
u64 prev_idle_time = 0;
u64 prev_iowait_time = 0;
u64 prev_steal_time = 0;
u64 prev_guest_time = 0;
u64 prev_idle_jiffies = 0;
u64 prev_nonIdle_jiffies = 0;

static void collect_cpu_stats(struct seq_file *m) {
    int cpu;
    // Para cada cpu realizamos as seguintes coletas de dados (no nosso caso, nossa VM utiliza apenas a CPU 0)
    for_each_online_cpu(cpu) {

        // Coleta dos tempos da CPU
        u64 user_time = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
        u64 system_time = kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
        u64 softirq_time = kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
        u64 irq_time = kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
        u64 idle_time = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
        u64 iowait_time = kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
        u64 steal_time = kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
        u64 guest_time = kcpustat_cpu(cpu).cpustat[CPUTIME_GUEST];

        // Cálculo do tempo não ocioso da CPU
        u64 nonIdle_jiffies = user_time + system_time + softirq_time + irq_time + steal_time + guest_time;
        //Cálculo do tempo ocioso da CPU
        u64 idle_jiffies = idle_time + iowait_time;

        // Cálculo do tempo total de uso da CPU, sendo o prévio e o atual respectivamente
        u64 prev_total_jiffies = prev_idle_jiffies + prev_nonIdle_jiffies;
        u64 total_jiffies = idle_jiffies + nonIdle_jiffies;

        // Cálculo do uso da CPU a partir da diferença: Tempo_Atual - Tempo_Previo
        u64 idle_percentage_fixed = ((idle_jiffies - prev_idle_jiffies) * 100) / (total_jiffies - prev_total_jiffies);
        u64 cpu_percentage = 100 - idle_percentage_fixed;

        // Salvamos as informações coletadas no buffer do seq_file m
        seq_printf(m, "\n  >>>   CPU STATS   <<<\n  ==========================================================================\n");
        seq_printf(m, "  CPU %d use percentage: %llu%%\n\n", cpu, cpu_percentage);

        // Atualização do tempo prévio
        prev_user_time = user_time;
        prev_system_time = system_time;
        prev_softirq_time = softirq_time;
        prev_irq_time = irq_time;
        prev_idle_time = idle_time;
        prev_iowait_time = iowait_time;
        prev_steal_time = steal_time;
        prev_guest_time = guest_time;
        prev_idle_jiffies = idle_jiffies;
        prev_nonIdle_jiffies = nonIdle_jiffies;
    }
}

static void collect_memory_stats(struct seq_file *m) {
    struct sysinfo i; // Struct que possui as varáiveis de memória que serão preenchidas
    unsigned long total_ram, free_ram, shared_ram, buffer_ram;

    si_meminfo(&i); // Coleta as informaçoes da memória e preenche a struct

    // Formatamos os dados para que fiquem em KB
    total_ram = i.totalram << (PAGE_SHIFT - 10);
    free_ram = i.freeram << (PAGE_SHIFT - 10);
    shared_ram = i.sharedram << (PAGE_SHIFT - 10);
    buffer_ram = i.bufferram << (PAGE_SHIFT - 10);

    // Salvamos as informações coletadas no buffer do seq_file m
    seq_printf(m, "\n  >>>   MEMORY STATS   <<<\n  ==========================================================================\n");
    seq_printf(m, "  Total RAM: %lu KB\n", total_ram);
    seq_printf(m, "  Free RAM: %lu KB\n", free_ram);
    seq_printf(m, "  Shared RAM: %lu KB\n", shared_ram);
    seq_printf(m, "  Buffer RAM: %lu KB\n", buffer_ram);
}

static void collect_disk_stats(struct seq_file *m) {
    struct file *file;
    char *buf, *line, *ptr;
    loff_t pos = 0;
    ssize_t read_size;

    // Acessamos o arquivo /proc/diskstats para leitura e atribuimos à file
    file = filp_open("/proc/diskstats", O_RDONLY, 0);
    if (IS_ERR(file)) {
        pr_err("Kernel Resource Monitor: failed to open /proc/diskstats\n");
        return;
    }

    // Alocamos memória para o buffer
    buf = kmalloc(4096, GFP_KERNEL);
    if (!buf) {
        pr_err("Kernel Resource Monitor: failed to allocate memory\n");
        filp_close(file, NULL);
        return;
    }

    // Realizamos a leitura do arquivo atribuido em file
    read_size = kernel_read(file, buf, 4096, &pos);
    if (read_size < 0) {
        pr_err("Kernel Resource Monitor: failed to read /proc/diskstats\n");
        kfree(buf);
        filp_close(file, NULL);
        return;
    }

    // Garantimos que o buffer é um vetor de caracteres válido
    buf[read_size] = '\0';

    // iteramos sobre o buffer através de ptr
    ptr = buf;
    seq_printf(m, "\n\n  >>>   DISK STATS   <<<\n  ==========================================================================\n");
    // Em cada interção, atribuimos a line informações sobre cada partição (Formatação das informações do arquivo /diskstats)
    while ((line = strsep(&ptr, "\n")) != NULL) {
        if (*line == '\0')
            continue;

        int major, minor;
        char device[32];
        unsigned long reads, reads_merged, sectors_read, time_reading;
        unsigned long writes, writes_merged, sectors_written, time_writing;
        unsigned long io_in_progress, time_io, weighted_time_io;

        // Separamos as informações salvas em line em variáveis diferentes
        sscanf(line, "%d %d %s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
               &major, &minor, device, &reads, &reads_merged, &sectors_read, &time_reading,
               &writes, &writes_merged, &sectors_written, &time_writing,
               &io_in_progress, &time_io, &weighted_time_io);

        // Salvamos as informações coletadas no buffer do seq_file m
        seq_printf(m, "  Device: %s\n", device);
        seq_printf(m, "  Major: %d, Minor: %d\n", major, minor);
        seq_printf(m, "  Reads completed: %lu\n", reads);
        seq_printf(m, "  Reads merged: %lu\n", reads_merged);
        seq_printf(m, "  Sectors read: %lu\n", sectors_read);
        seq_printf(m, "  Time spent reading (ms): %lu\n", time_reading);
        seq_printf(m, "  Writes completed: %lu\n", writes);
        seq_printf(m, "  Writes merged: %lu\n", writes_merged);
        seq_printf(m, "  Sectors written: %lu\n", sectors_written);
        seq_printf(m, "  Time spent writing (ms): %lu\n", time_writing);
        seq_printf(m, "  I/Os currently in progress: %lu\n", io_in_progress);
        seq_printf(m, "  Time spent doing I/Os (ms): %lu\n", time_io);
        seq_printf(m, "  Weighted time spent doing I/Os (ms): %lu\n\n", weighted_time_io);
    }

    // Liberamos a memória alocada para o buffer e fechamos o arquivo
    kfree(buf);
    filp_close(file, NULL);
}

static void monitor_timer_callback(struct timer_list *t) {
    struct seq_file m; // Instancia seq_file para salvar as informações sequencialmente no buffer presente na struct

    mutex_lock(&stats_buffer_mutex);

    // Criamos um mutex de acesso ao buffer para que ele não seja acessado enquanto estiver vazio

    if (stats_buffer) { // Garante que qualquer memória previamente alocada seja liberada antes de alocarmos o buffer
        kfree(stats_buffer);
        stats_buffer = NULL;
    }

    stats_buffer = kmalloc(stats_buffer_size, GFP_KERNEL); // alocamos memória para o buffer
    if (!stats_buffer) {
        pr_err("Kernel Resource Monitor: failed to allocate memory\n");
        mutex_unlock(&stats_buffer_mutex);
        return;
    }

    memset(&m, 0, sizeof(m));
    m.buf = stats_buffer;
    m.size = stats_buffer_size; // Atribuimos o buffer e seu tamanho à struct

    collect_cpu_stats(&m); // Coletamos as informações da cpu
    collect_memory_stats(&m); // Coletamos as informações da memória
    collect_disk_stats(&m); // Coletamos as informações do disco

    m.buf[m.count] = '\0'; // Garantimos que o buffer é um vetor de caracteres válido

    mutex_unlock(&stats_buffer_mutex);

    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(MONITOR_INTERVAL * 1000)); // rearma o timer
}

static int krm_show(struct seq_file *m, void *v) {
    // Controle de acesso ao buffer através de um mutex
    mutex_lock(&stats_buffer_mutex);
    if (stats_buffer) {
    // Caso o buffer de seq_file nao seja vazio, pritamos na tela suas informações
        seq_printf(m, "%s", stats_buffer);
    }
    mutex_unlock(&stats_buffer_mutex);
    return 0;
}

static int krm_open(struct inode *inode, struct file *file) {
    // Abre o arquivo e chamamos a função krm_show para mostrar os dados coletados
    return single_open(file, krm_show, NULL);
}

// Opções de ação para o arquivo criado (krm_stats)
static const struct proc_ops krm_ops = {
    .proc_open = krm_open, // Caso a ação seja "open", a função krm_open é chamada
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init krm_init(void) {
    pr_info("Kernel Resource Monitor: Initializing\n");

    proc_entry = proc_create("krm_stats", 0, NULL, &krm_ops);       // cria um arquivo "krm_stats" dentro do /proc
    if (!proc_entry) {                                              // verificação de erro da criação do novo arquivo
        pr_err("Kernel Resource Monitor: failed to create /proc/krm_stats\n");
        return -ENOMEM;
    }

    timer_setup(&monitor_timer, monitor_timer_callback, 0);         // inicializa o timer (coleta acontece a cada 1s)
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(MONITOR_INTERVAL * 1000)); // rearma o timer

    return 0;
}

static void __exit krm_exit(void) {
    del_timer(&monitor_timer); // deletamos o timer
    proc_remove(proc_entry); // Finalizamos o acesso a /proc e exluimos o arquivo krm_stats

    mutex_lock(&stats_buffer_mutex);
    if (stats_buffer) {
        kfree(stats_buffer); // Caso o buffer não seja vazio, liberamos a memória alocada para ele
        stats_buffer = NULL; // buffer é esvaziado
    }
    mutex_unlock(&stats_buffer_mutex);

    pr_info("Kernel Resource Monitor: Exiting\n");
}

module_init(krm_init);
module_exit(krm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel Resource Monitor Module");
