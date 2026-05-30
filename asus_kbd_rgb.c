/*
 * asus_kbd_rgb.c — Controlador RGB do teclado ASUS VivoBook S14 (S5406)
 *
 * Hardware: ITE5570  VID=0x0B05  PID=0x19B6
 * Backlight de zona única — efeitos implementados por software via pthreads.
 *
 * Modos disponíveis:
 *   static  — cor fixa
 *   breathe — pulsa suavemente a cor escolhida
 *   cycle   — percorre todo o espectro HSV
 *   off     — apaga o backlight
 *
 * Compilar:
 *   gcc -O2 -Wall -o asus_kbd_rgb asus_kbd_rgb.c -lpthread -lm
 *
 * Permissão sem root:
 *   echo 'KERNEL=="hidraw*", ATTRS{idVendor}=="0b05", ATTRS{idProduct}=="19b6", MODE="0660", GROUP="input"' \
 *     | sudo tee /etc/udev/rules.d/99-asus-kbd-rgb.rules
 *   sudo udevadm control --reload && sudo udevadm trigger
 *   sudo usermod -aG input $USER
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <glob.h>
#include <ctype.h>

/* ── IDs do hardware ─────────────────────────────────────────── */
#define ASUS_VENDOR_ID   0x0B05
static const uint16_t KNOWN_PIDS[] = { 0x19B6, 0x1B2C, 0 };

/* ── ioctl helpers ───────────────────────────────────────────── */
#define HIDIOCSFEATURE(len)  _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#define HIDIOCGFEATURE(len)  _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)

/* ── Tamanhos dos reports (extraídos do descriptor real) ─────── */
#define RPT_CTRL_SIZE      2   /* 0x0B LampArrayControl  */
#define RPT_RANGE_SIZE    10   /* 0x05 LampRangeUpdate   */
#define RPT_ARRAY_SIZE    23   /* 0x01 LampArrayAttr     */

#define RPT_LAMP_ARRAY_ATTR  0x01
#define RPT_RANGE_UPDATE     0x05
#define RPT_ARRAY_CTRL       0x0B

/* ── Modos suportados (zona única) ───────────────────────────── */
typedef enum {
    MODE_STATIC = 0,
    MODE_BREATHE,
    MODE_CYCLE,
    MODE_OFF,
    MODE_COUNT
} KbdMode;
static const char *MODE_NAMES[] = { "static", "breathe", "cycle", "off" };

typedef enum { SPEED_SLOW=0, SPEED_MED, SPEED_FAST } KbdSpeed;
static const char *SPEED_NAMES[] = { "slow", "med", "fast" };

typedef struct {
    uint8_t  r, g, b;
    KbdMode  mode;
    KbdSpeed speed;
    uint8_t  brightness; /* 0–3 */
} KbdConfig;

/* ── Estado global ───────────────────────────────────────────── */
static volatile bool   anim_running = false;
static pthread_t       anim_thread;
static pthread_mutex_t anim_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_fd = -1;
static KbdConfig       g_cfg;

/* ── Utilitários ─────────────────────────────────────────────── */
static bool parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s[0]=='\'' || s[0]=='"') s++;
    if (s[0]=='#') s++;
    char hex[7]; int len=0;
    while (len<6 && isxdigit((unsigned char)s[len])) { hex[len]=s[len]; len++; }
    if (len!=6) return false;
    hex[6]='\0';
    unsigned int v; sscanf(hex, "%06x", &v);
    *r=(v>>16)&0xFF; *g=(v>>8)&0xFF; *b=v&0xFF;
    return true;
}

/* Converte HSV para RGB. h=0..360, s/v=0..1 */
static void hsv_to_rgb(float h, float s, float v,
                        uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c=v*s, x=c*(1.0f-fabsf(fmodf(h/60.0f,2.0f)-1.0f)), m=v-c;
    float rf=0,gf=0,bf=0;
    if      (h< 60){rf=c;gf=x;}
    else if (h<120){rf=x;gf=c;}
    else if (h<180){gf=c;bf=x;}
    else if (h<240){gf=x;bf=c;}
    else if (h<300){rf=x;bf=c;}
    else           {rf=c;bf=x;}
    *r=(uint8_t)((rf+m)*255.0f);
    *g=(uint8_t)((gf+m)*255.0f);
    *b=(uint8_t)((bf+m)*255.0f);
}

/* ── Detecção do hidraw ──────────────────────────────────────── */
static int find_hidraw_device(bool verbose)
{
    glob_t gl;
    if (glob("/sys/class/hidraw/hidraw*/device/uevent",0,NULL,&gl)!=0) {
        fprintf(stderr,"[erro] Nenhum dispositivo hidraw encontrado.\n");
        return -1;
    }
    for (size_t i=0; i<gl.gl_pathc; i++) {
        FILE *f=fopen(gl.gl_pathv[i],"r"); if(!f) continue;
        char line[256]; uint32_t vid=0,pid=0;
        while(fgets(line,sizeof(line),f))
            if(strncmp(line,"HID_ID=",7)==0) sscanf(line+7,"%*x:%x:%x",&vid,&pid);
        fclose(f);
        if(vid!=ASUS_VENDOR_ID) continue;
        bool match=false;
        for(int j=0;KNOWN_PIDS[j];j++) if(pid==KNOWN_PIDS[j]){match=true;break;}
        if(!match) continue;
        const char *p=strstr(gl.gl_pathv[i],"hidraw/");
        if(!p) continue;
        p+=strlen("hidraw/");
        char *slash=strchr(p,'/'); if(!slash) continue;
        char devname[64];
        snprintf(devname,sizeof(devname),"/dev/%.*s",(int)(slash-p),p);
        if(verbose)
            printf("[info] Controlador: %s (VID=0x%04X PID=0x%04X)\n",devname,vid,pid);
        globfree(&gl);
        return open(devname,O_RDWR);
    }
    globfree(&gl);
    fprintf(stderr,"[erro] ITE5570 não encontrado. Verifique /dev/hidraw*.\n");
    return -1;
}

/* ── Feature reports ─────────────────────────────────────────── */
static bool send_feature(int fd, uint8_t *buf, size_t len)
{
    if(ioctl(fd,HIDIOCSFEATURE(len),buf)<0){ perror("[erro] HIDIOCSFEATURE"); return false; }
    return true;
}
static bool get_feature(int fd, uint8_t *buf, size_t len)
{
    if(ioctl(fd,HIDIOCGFEATURE(len),buf)<0){ perror("[erro] HIDIOCGFEATURE"); return false; }
    return true;
}

/* ── Primitivas HID ──────────────────────────────────────────── */

/*
 * Passa o controle para o host (false) ou devolve ao firmware (true).
 * DEVE ser chamado com false antes de qualquer update de cor.
 */
static bool kbd_set_autonomous(int fd, bool autonomous)
{
    uint8_t buf[RPT_CTRL_SIZE]={RPT_ARRAY_CTRL, autonomous?0x01:0x00};
    return send_feature(fd,buf,RPT_CTRL_SIZE);
}

/*
 * Envia cor para todas as lâmpadas de uma vez (range 0x0000–0xFFFF).
 * Pressupõe que kbd_set_autonomous(false) já foi chamado.
 */
static bool kbd_set_color_raw(int fd, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t buf[RPT_RANGE_SIZE]={
        RPT_RANGE_UPDATE, 0x01,
        0x00,0x00,   /* LampIdStart = 0      */
        0xFF,0xFF,   /* LampIdEnd   = 0xFFFF */
        r, g, b, 0xFF
    };
    return send_feature(fd,buf,RPT_RANGE_SIZE);
}

static int kbd_get_lamp_count(int fd)
{
    uint8_t buf[RPT_ARRAY_SIZE]={RPT_LAMP_ARRAY_ATTR};
    if(!get_feature(fd,buf,RPT_ARRAY_SIZE)) return -1;
    return buf[1]|(buf[2]<<8);
}

/* ── API de alto nível ───────────────────────────────────────── */
static bool kbd_turn_off(int fd)
{
    kbd_set_autonomous(fd,false);
    usleep(20000);
    return kbd_set_color_raw(fd,0,0,0);
}

static bool kbd_turn_on(int fd)
{
    kbd_set_autonomous(fd,false);
    usleep(20000);
    return kbd_set_color_raw(fd,0xFF,0xFF,0xFF);
}

/* ── Threads de animação ─────────────────────────────────────── */
static void anim_stop(void)
{
    if(!anim_running) return;
    anim_running=false;
    pthread_join(anim_thread,NULL);
}

/*
 * breathe: sobe suavemente de 0 → brilho_max → 0 usando curva senoidal.
 * A cor base e o brilho são relidos do g_cfg a cada frame para permitir
 * ajuste em tempo real pelo menu interativo.
 */
static void *anim_breathe(void *arg)
{
    (void)arg;
    /* brilho máximo atingido no pico (brightness=0 → 15%, 1→33%, 2→66%, 3→100%) */
    static const float bscale[]    = { 0.15f, 0.33f, 0.66f, 1.0f };
    /* passos por metade do ciclo (subida ou descida) por velocidade */
    static const int   half_steps[]= { 80, 50, 28 };
    /* pausa entre frames (µs) — define duração do ciclo */
    static const int   frame_us[]  = { 18000, 12000, 7000 };

    while (anim_running) {
        pthread_mutex_lock(&anim_mutex);
        KbdConfig cfg = g_cfg;
        pthread_mutex_unlock(&anim_mutex);

        int steps   = half_steps[cfg.speed];
        int fus     = frame_us[cfg.speed];

        /* subida: 0 → pico */
        for (int i=0; i<=steps && anim_running; i++) {
            float t = (float)i / (float)steps;
            /* sin de 0 a π/2: sobe suavemente de 0 a 1 */
            float intensity = sinf(t * (float)(M_PI / 2.0));

            pthread_mutex_lock(&anim_mutex);
            cfg = g_cfg;
            pthread_mutex_unlock(&anim_mutex);
            float base = bscale[cfg.brightness < 4 ? cfg.brightness : 3];

            uint8_t r=(uint8_t)(cfg.r * base * intensity);
            uint8_t g=(uint8_t)(cfg.g * base * intensity);
            uint8_t b=(uint8_t)(cfg.b * base * intensity);
            kbd_set_color_raw(g_fd, r, g, b);
            usleep((unsigned)fus);
        }

        /* descida: pico → 0 */
        for (int i=steps; i>=0 && anim_running; i--) {
            float t = (float)i / (float)steps;
            float intensity = sinf(t * (float)(M_PI / 2.0));

            pthread_mutex_lock(&anim_mutex);
            cfg = g_cfg;
            pthread_mutex_unlock(&anim_mutex);
            float base = bscale[cfg.brightness < 4 ? cfg.brightness : 3];

            uint8_t r=(uint8_t)(cfg.r * base * intensity);
            uint8_t g=(uint8_t)(cfg.g * base * intensity);
            uint8_t b=(uint8_t)(cfg.b * base * intensity);
            kbd_set_color_raw(g_fd, r, g, b);
            usleep((unsigned)fus);
        }

        /* pausa no escuro antes do próximo ciclo */
        for (int i=0; i<12 && anim_running; i++)
            usleep(20000);
    }
    return NULL;
}

/*
 * cycle: percorre o espectro HSV completo mantendo saturação=1
 * e brilho controlado pela config.
 */
static void *anim_cycle(void *arg)
{
    (void)arg;
    static const float bscale[]   = { 0.15f, 0.33f, 0.66f, 1.0f };
    /* graus por frame por velocidade */
    static const float deg_step[] = { 0.4f, 0.8f, 1.6f };
    /* pausa entre frames (µs) */
    static const int   frame_us[] = { 20000, 14000, 8000 };

    float hue = 0.0f;

    while (anim_running) {
        pthread_mutex_lock(&anim_mutex);
        KbdConfig cfg = g_cfg;
        pthread_mutex_unlock(&anim_mutex);

        float base  = bscale[cfg.brightness < 4 ? cfg.brightness : 3];
        float step  = deg_step[cfg.speed];
        int   fus   = frame_us[cfg.speed];

        uint8_t r,g,b;
        hsv_to_rgb(hue, 1.0f, base, &r, &g, &b);
        kbd_set_color_raw(g_fd, r, g, b);

        hue += step;
        if (hue >= 360.0f) hue -= 360.0f;

        usleep((unsigned)fus);
    }
    return NULL;
}

static bool anim_start(int fd, const KbdConfig *cfg)
{
    anim_stop();
    g_fd = fd;
    pthread_mutex_lock(&anim_mutex);
    g_cfg = *cfg;
    pthread_mutex_unlock(&anim_mutex);

    /* garantir controle do host antes de começar a animar */
    kbd_set_autonomous(fd, false);
    usleep(20000);

    void *(*fn)(void*)=NULL;
    if(cfg->mode==MODE_BREATHE) fn=anim_breathe;
    if(cfg->mode==MODE_CYCLE)   fn=anim_cycle;
    if(!fn) return false;

    anim_running=true;
    if(pthread_create(&anim_thread,NULL,fn,NULL)!=0){
        anim_running=false;
        perror("[erro] pthread_create");
        return false;
    }
    return true;
}

/* ── Aplica config completa ──────────────────────────────────── */
static bool kbd_apply_config(int fd, const KbdConfig *cfg)
{
    anim_stop();

    static const float bscale[]={0.15f,0.33f,0.66f,1.0f};
    float s=bscale[cfg->brightness<4?cfg->brightness:3];

    switch(cfg->mode){
    case MODE_OFF:
        return kbd_turn_off(fd);

    case MODE_STATIC:
        kbd_set_autonomous(fd,false);
        usleep(20000);
        return kbd_set_color_raw(fd,
            (uint8_t)(cfg->r*s),
            (uint8_t)(cfg->g*s),
            (uint8_t)(cfg->b*s));

    case MODE_BREATHE:
    case MODE_CYCLE:
        return anim_start(fd,cfg);

    default: return false;
    }
}

/* ── Debug ───────────────────────────────────────────────────── */
static void kbd_debug(int fd)
{
    printf("\n=== Debug: informações do controlador ===\n");
    struct hidraw_devinfo info;
    if(ioctl(fd,HIDIOCGRAWINFO,&info)==0){
        printf("  Bus:        0x%04X\n",info.bustype);
        printf("  Vendor ID:  0x%04X\n",(uint16_t)info.vendor);
        printf("  Product ID: 0x%04X\n",(uint16_t)info.product);
    }
    char name[256]={0};
    if(ioctl(fd,HIDIOCGRAWNAME(sizeof(name)),name)>=0)
        printf("  Nome HID:   %s\n",name);
    int lamps=kbd_get_lamp_count(fd);
    if(lamps>0) printf("  Lâmpadas:   %d\n",lamps);
    else        printf("  Lâmpadas:   (não lido)\n");
    printf("=========================================\n\n");
}

/* ── Banner / Ajuda ──────────────────────────────────────────── */
static void print_banner(void)
{
    printf("\n");
    printf("  ╔═══════════════════════════════════════════════════╗\n");
    printf("  ║  ASUS VivoBook S14 S5406 — RGB Teclado (Linux)   ║\n");
    printf("  ╚═══════════════════════════════════════════════════╝\n\n");
}

static void print_help(void)
{
    printf("Uso:\n");
    printf("  asus_kbd_rgb [opções]\n\n");
    printf("Opções:\n");
    printf("  --color <cor>        Cor hex: FF0000 ou \"#FF0000\"\n");
    printf("  --mode  <modo>       static | breathe | cycle | off\n");
    printf("  --speed <vel>        slow | med | fast  (breathe e cycle)\n");
    printf("  --brightness <0-3>   Nível de brilho\n");
    printf("  --on                 Liga com branco brilho máximo\n");
    printf("  --off                Desliga o backlight\n");
    printf("  --debug              Informações do hardware\n");
    printf("  --interactive        Menu interativo\n");
    printf("  --help               Esta ajuda\n\n");
    printf("Exemplos:\n");
    printf("  sudo asus_kbd_rgb --color 00FF88 --brightness 3\n");
    printf("  sudo asus_kbd_rgb --mode breathe --color FF6600 --speed slow\n");
    printf("  sudo asus_kbd_rgb --mode cycle --speed fast\n");
    printf("  sudo asus_kbd_rgb --off\n");
    printf("  sudo asus_kbd_rgb --interactive\n\n");
    printf("Nota: modos breathe e cycle ficam rodando até Ctrl+C.\n\n");
}

/* ── Sinal ───────────────────────────────────────────────────── */
static void handle_sigint(int sig)
{
    (void)sig;
    anim_stop();
    if(g_fd>=0) kbd_turn_off(g_fd);
    exit(0);
}

/* ── Menu interativo ─────────────────────────────────────────── */
static void interactive_menu(int fd)
{
    KbdConfig cfg={.r=255,.g=255,.b=255,.mode=MODE_STATIC,.speed=SPEED_MED,.brightness=3};
    print_banner();
    printf("  Modo INTERATIVO — 'q' para sair\n\n");

    while(1){
        const char *anim_tag = anim_running ? " [animando]" : "";
        printf("┌────────────────────────────────────────────────┐\n");
        printf("│ Cor: #%02X%02X%02X  Modo: %-8s  Brilho: %d/3%s\n",
               cfg.r,cfg.g,cfg.b,MODE_NAMES[cfg.mode],cfg.brightness,anim_tag);
        printf("│ Velocidade: %-4s  (para breathe e cycle)       │\n",
               SPEED_NAMES[cfg.speed]);
        printf("└────────────────────────────────────────────────┘\n");
        printf("  1)Cor  2)Modo  3)Velocidade  4)Brilho\n");
        printf("  5)Aplicar  6)Off  7)On  8)Debug  q)Sair\n");
        printf("> "); fflush(stdout);

        char line[128];
        if(!fgets(line,sizeof(line),stdin)) break;
        line[strcspn(line,"\n")]=0;
        if(strcmp(line,"q")==0) break;

        switch(line[0]){
        case '1':{
            printf("Cor (RRGGBB ou #RRGGBB): "); fflush(stdout);
            char col[16]; if(!fgets(col,sizeof(col),stdin)) break;
            col[strcspn(col,"\n")]=0;
            uint8_t r,g,b;
            if(parse_hex_color(col,&r,&g,&b)){
                cfg.r=r; cfg.g=g; cfg.b=b;
                pthread_mutex_lock(&anim_mutex); g_cfg=cfg; pthread_mutex_unlock(&anim_mutex);
                /* se static, aplicar imediatamente */
                if(!anim_running) kbd_apply_config(fd,&cfg);
                printf("[ok] Cor #%02X%02X%02X\n",r,g,b);
            } else printf("[erro] Use formato RRGGBB (ex: FF3300)\n");
            break;
        }
        case '2':{
            printf("Modos disponíveis:\n");
            for(int i=0;i<MODE_COUNT;i++) printf("  %d) %s\n",i,MODE_NAMES[i]);
            printf("Escolha (0-%d): ",MODE_COUNT-1); fflush(stdout);
            char m[8]; if(!fgets(m,sizeof(m),stdin)) break;
            int v=atoi(m);
            if(v>=0&&v<MODE_COUNT){ cfg.mode=(KbdMode)v; printf("[ok] Modo: %s\n",MODE_NAMES[v]); }
            break;
        }
        case '3':{
            printf("Velocidade (0=slow 1=med 2=fast): "); fflush(stdout);
            char sp[8]; if(!fgets(sp,sizeof(sp),stdin)) break;
            int v=atoi(sp);
            if(v>=0&&v<=2){
                cfg.speed=(KbdSpeed)v;
                pthread_mutex_lock(&anim_mutex); g_cfg=cfg; pthread_mutex_unlock(&anim_mutex);
                printf("[ok] Velocidade: %s\n",SPEED_NAMES[v]);
            }
            break;
        }
        case '4':{
            printf("Brilho (0-3): "); fflush(stdout);
            char br[8]; if(!fgets(br,sizeof(br),stdin)) break;
            int v=atoi(br);
            if(v>=0&&v<=3){
                cfg.brightness=(uint8_t)v;
                pthread_mutex_lock(&anim_mutex); g_cfg=cfg; pthread_mutex_unlock(&anim_mutex);
                /* se static, aplicar imediatamente */
                if(!anim_running) kbd_apply_config(fd,&cfg);
                printf("[ok] Brilho %d aplicado.\n",v);
            } else printf("[erro] Use 0, 1, 2 ou 3.\n");
            break;
        }
        case '5':
            if(kbd_apply_config(fd,&cfg)){
                if(cfg.mode==MODE_BREATHE||cfg.mode==MODE_CYCLE)
                    printf("[ok] Animação iniciada! Velocidade e cor ajustáveis sem parar.\n");
                else
                    printf("[ok] Aplicado!\n");
            } else printf("[erro] Falha ao aplicar.\n");
            break;
        case '6':
            anim_stop();
            printf(kbd_turn_off(fd)?"[ok] Backlight desligado.\n":"[erro]\n");
            break;
        case '7':
            anim_stop();
            printf(kbd_turn_on(fd)?"[ok] Backlight ligado (branco).\n":"[erro]\n");
            break;
        case '8': kbd_debug(fd); break;
        default: printf("Comando desconhecido.\n");
        }
        printf("\n");
    }

    anim_stop();
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if(argc<2){ print_banner(); print_help(); return 0; }

    KbdConfig cfg={.r=255,.g=255,.b=255,.mode=MODE_STATIC,.speed=SPEED_MED,.brightness=3};
    bool do_off=false,do_on=false,do_debug=false,do_interactive=false,do_apply=false,verbose=false;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){
            print_banner(); print_help(); return 0;
        } else if(!strcmp(argv[i],"--color")&&i+1<argc){
            uint8_t r,g,b;
            if(!parse_hex_color(argv[++i],&r,&g,&b)){ fprintf(stderr,"[erro] Cor inválida. Use RRGGBB\n"); return 1; }
            cfg.r=r; cfg.g=g; cfg.b=b; do_apply=true;
        } else if(!strcmp(argv[i],"--mode")&&i+1<argc){
            bool found=false; i++;
            for(int j=0;j<MODE_COUNT;j++) if(!strcmp(argv[i],MODE_NAMES[j])){cfg.mode=(KbdMode)j;found=true;break;}
            if(!found){ fprintf(stderr,"[erro] Modo '%s' inválido. Use: static breathe cycle off\n",argv[i]); return 1; }
            do_apply=true;
        } else if(!strcmp(argv[i],"--speed")&&i+1<argc){
            i++;
            if     (!strcmp(argv[i],"slow")) cfg.speed=SPEED_SLOW;
            else if(!strcmp(argv[i],"med"))  cfg.speed=SPEED_MED;
            else if(!strcmp(argv[i],"fast")) cfg.speed=SPEED_FAST;
            else{ fprintf(stderr,"[erro] Velocidade inválida. Use: slow med fast\n"); return 1; }
        } else if(!strcmp(argv[i],"--brightness")&&i+1<argc){
            int v=atoi(argv[++i]);
            if(v<0||v>3){ fprintf(stderr,"[erro] Brilho deve ser 0-3.\n"); return 1; }
            cfg.brightness=(uint8_t)v; do_apply=true;
        } else if(!strcmp(argv[i],"--off"))         do_off=true;
          else if(!strcmp(argv[i],"--on"))          do_on=true;
          else if(!strcmp(argv[i],"--debug"))       { do_debug=true; verbose=true; }
          else if(!strcmp(argv[i],"--interactive")) do_interactive=true;
          else if(!strcmp(argv[i],"--verbose")||!strcmp(argv[i],"-v")) verbose=true;
          else fprintf(stderr,"[aviso] Opção desconhecida: %s\n",argv[i]);
    }

    int fd=find_hidraw_device(verbose);
    if(fd<0) return 1;
    g_fd=fd;

    signal(SIGINT, handle_sigint);
    signal(SIGTERM,handle_sigint);

    int ret=0;

    if(do_debug) kbd_debug(fd);

    if(do_interactive){
        interactive_menu(fd);
    } else if(do_off){
        if(!kbd_turn_off(fd)){ fprintf(stderr,"[erro] Falha ao desligar.\n"); ret=1; }
        else if(verbose) printf("[ok] Backlight desligado.\n");
    } else if(do_on){
        if(!kbd_turn_on(fd)){ fprintf(stderr,"[erro] Falha ao ligar.\n"); ret=1; }
        else if(verbose) printf("[ok] Backlight ligado.\n");
    } else if(do_apply){
        if(!kbd_apply_config(fd,&cfg)){
            fprintf(stderr,"[erro] Falha ao aplicar.\n"); ret=1;
        } else {
            if(verbose)
                printf("[ok] Cor=#%02X%02X%02X modo=%s brilho=%d\n",
                       cfg.r,cfg.g,cfg.b,MODE_NAMES[cfg.mode],cfg.brightness);
            if(cfg.mode==MODE_BREATHE||cfg.mode==MODE_CYCLE){
                printf("Animação rodando... Ctrl+C para parar e apagar.\n");
                pthread_join(anim_thread,NULL);
            }
        }
    } else if(!do_debug){
        print_banner(); print_help();
    }

    anim_stop();
    close(fd);
    return ret;
}
