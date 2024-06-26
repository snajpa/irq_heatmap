#include "irq_numa.h"

// yes, I know this is bad
#define IRQ_PATH_MAX 4096

// global for now 
struct numa_topology topology; 

// ask cpu how many siblings it has. This assumes a symmetrical machine 
int irqnuma_num_hyperthreads()
{
     FILE *fp;
     int i = 1;
     char ch;
     
     if ((fp = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list","r")) != NULL) {
	  while ((ch=fgetc(fp))!=EOF) if (ch == ',') i++;
     }
     fclose(fp);
     return i;
}

// read a simple integer from a sysfs path
int irqnuma_sysfs_integer(char *path)
{
     int fd, number;
     
     number = -1;
     
     if ((fd = open(path,O_RDONLY)) >= 0) {
	  char buffer[8];
	  int rt;
	  
	  if ((rt = read(fd,buffer,8)) >= 0) {
	       number = strtol(buffer,NULL,10);
	  }
     }
     close(fd);
     return number;
}
     
// read a bitmask. The format for the direct bitmaps varies between kernels. In some (2.6.32) a 
// hex number is returned. In others (4.4.60) a direct representation of the bitmask is returned. 
// So we parse the human readable form. 
struct bitmask *irqnuma_sysfs_cpustring(char *path)
{
     int fd; 
     char buffer[4096];
     struct bitmask *b;
     
     b=NULL;
     memset(buffer,0,4096);
     
     if ((fd = open(path,O_RDONLY)) >= 0) {
	  int rt;
	  if ((rt = read(fd,buffer,4096)) >= 0) {
	       buffer[rt-1]='\0'; // trim the '\n' off the end. 
	       b = numa_parse_cpustring(buffer);
	  }
     }
     close(fd);
     return b;
}

int irqnuma_core_online(int cpuid)
{
     char *format = "/sys/devices/system/cpu/cpu%d/online";
     char file[IRQ_PATH_MAX];
     
     sprintf(file,format,cpuid);
     return irqnuma_sysfs_integer(file);
}

int irqnuma_get_coreid(int cpuid)
{
     char *format = "/sys/devices/system/cpu/cpu%d/topology/core_id";
     char file[IRQ_PATH_MAX];
     
     sprintf(file,format,cpuid);
     return irqnuma_sysfs_integer(file);
}

int irqnuma_get_threadid(int cpuid)
{
     char *format = "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list";
     char file[IRQ_PATH_MAX];
     struct bitmask *cpumask;
     int i, j;
     
     sprintf(file,format,cpuid);
     cpumask = irqnuma_sysfs_cpustring(file);
     
     if (cpumask == 0) {
	  fprintf(stderr,"irqnuma: unable to read the thread siblings list for cpu %d\n",cpuid);
	  exit(-1);
     }
     
//     cpu_count = numa_num_configured_cpus();
//     printf ("bitmask size %d, cpu count = %d\n",cpumask->size,cpu_count);
     
     j=0;
     for (i=0;i<cpuid;i++) {
	  if (numa_bitmask_isbitset(cpumask,i)) j++;
     }
     
     return j;
}

void irqnuma_add_cpu_to_topology(int cpuid,int socket, int coreid, int thread_id) 
{
     int cc,tc;
     
     if (thread_id >= (MAX_THREADS-1)) {
	  fprintf(stderr,"bug: recompile with increased max threads. current max_threads = %d\n",MAX_THREADS);
	  exit(-1);
     }
     
     cc = topology.map[socket].threads[thread_id].core_count;
     if (cc >= (MAX_CORES-1)) {
	  fprintf(stderr,"bug: recompile with increased max cores. current max_cores = %d\n",MAX_CORES);
	  exit(-1);
     }

     topology.map[socket].configured = 1;
     // threads 
     tc = topology.map[socket].thread_count;
     tc = (thread_id >= tc) ? thread_id+1 : tc;
     
     topology.map[socket].thread_count = tc;
     topology.map[socket].threads[thread_id].configured=1;
     topology.map[socket].threads[thread_id].core_count++;
     // cores
     // core id is junk on some machines. Not contiguous 
     topology.map[socket].threads[thread_id].cores[cc].core_id=coreid;
     topology.map[socket].threads[thread_id].cores[cc].cpu_id=cpuid;
     topology.map[socket].threads[thread_id].cores[cc].configured=1;
     return;
}
// This is the duration of USER_HZ (usually 1/100 th second or 10ms). 
int irqnuma_get_clocktick_ms()
{
    return 1000 / sysconf(_SC_CLK_TCK);
}

void irqnuma_init_topology()
{
     int i;

     if (numa_available() == -1) { 
	  fprintf(stderr,"numalib reports not available. Exiting\n");
	  exit (-1);
     }
     
     memset((void *)&topology,0,sizeof(struct numa_topology));
     
     // number of sockets 
     topology.number_of_sockets = numa_num_configured_nodes();
     if (topology.number_of_sockets >= MAX_SOCKETS) {
	  fprintf(stderr,"bug: recompile with increased max sockets. current max_sockets = %d, numalib says you have %d\n",MAX_SOCKETS,topology.number_of_sockets);
	  exit(-1);
     }

     // number of 'cpus'
     topology.number_of_cpus = numa_num_configured_cpus(); // includes disabled cpus. 

     // duration of a jiffy / clock tick
     topology.clock_tick_ms = irqnuma_get_clocktick_ms();
     
     // now we loop through the cpu list - which is hopefully contiguous 
     // and build our topology map. 
     for (i=0; i<topology.number_of_cpus; i++) {
	  // we need the socket physical package id, core id and cpu id and ht number 
       int online = irqnuma_core_online(i);
       if (!online) {
         continue;
       }
	  int node_id = numa_node_of_cpu(i); // could use numa_node_of_cpu ?? 
	  int core_id = irqnuma_get_coreid(i);
	  int thread_id = irqnuma_get_threadid(i);
	  // cpuid == i
	  irqnuma_add_cpu_to_topology(i,node_id,core_id,thread_id);
     }
     return;
}

void irqnuma_dump_topology()
{
     int s,c,t;
     
     fprintf(stderr,"Topology_Dump\n");
     fprintf(stderr,"topology.number_of_sockets = %d\n",topology.number_of_sockets);
     fprintf(stderr,"topology.number_of_cpus = %d\n",topology.number_of_cpus);
     fprintf(stderr,"topology.clock_tick_duration = %d\n",topology.clock_tick_ms);
     fprintf(stderr,"number of hyperthreads = %d\n",irqnuma_num_hyperthreads());
     
     for (s=0;s<topology.number_of_sockets;s++) {
	  fprintf(stderr,"Socket %d\n",s);
	  fprintf(stderr," configured %d\n",topology.map[s].configured);
	  fprintf(stderr," number of threads %d\n", topology.map[s].thread_count);
	  for (t=0;t<topology.map[s].thread_count;t++) {
	       fprintf(stderr,"  Thread %d\n",t);
	       fprintf(stderr,"  core count %d\n",topology.map[s].threads[t].core_count);
	       fprintf(stderr,"  configured %d\n",topology.map[s].threads[t].configured);
	       for (c=0;c< topology.map[s].threads[t].core_count; c++) {
		    fprintf(stderr,"   core %d, core_id %d, cpu id %d\n",c,topology.map[s].threads[t].cores[c].core_id,topology.map[s].threads[t].cores[c].cpu_id);
	       }
	  }
     }
}


#ifdef DEBUG

int main (int argc, char *argv[])
{
     int cpu_count,i;
     
     if (numa_available() == -1) {
	  printf ("numalib reports as unavailable\n");
	  exit(-1);
     }
     
     cpu_count = numa_num_configured_cpus();

     fprintf(stderr,"Raw Data Dump\n");
     fprintf(stderr,"cpu count %d, ht per physical core%d\n",cpu_count,irqnuma_num_hyperthreads());
     fprintf(stderr,"duration of a clock tick (jiffy) %d ms\n",irqnuma_get_clocktick_ms());
     fprintf(stderr,"Table Dump\ncpuid\tpackage\tcore\ttid\n");
     for (i=0;i<cpu_count;i++) {
	  fprintf(stderr,"%d\t%d\t%d\t%d\n",i,numa_node_of_cpu(i),irqnuma_get_coreid(i),irqnuma_get_threadid(i));
     }
     
     irqnuma_init_topology();

     irqnuma_dump_topology();
     
     return 0;
}
#endif    
