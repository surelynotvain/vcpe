#include <pspkernel.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <psppower.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include "vcpe.h"
#include "assets.h"
PSP_MODULE_INFO("VCPE Flat Perspective",0,1,0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER|THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
PSP_DISABLE_NEWLIB_SOCKET_SUPPORT();
static VcpeRayColumn columns[120];
static VcpeRayWall walls[64];
static unsigned wall_count,strips_drawn;
static const VcpeSectorCell cells[]={
 {-3,-8,3,0,0,3},{-3,-16,3,-8,0,3},{-3,-24,3,-16,0,3}};
static VcpeSectorPortal portals[]={
 {0,1,1,-1,-8,1,-8},{1,2,0,-1,-16,1,-16}};
static const VcpeSectors sectors={cells,3,portals,2};
static VcpeSectorBody player={0,0,-1,.22f,1.7f,.3f,.5f,0};
static float yaw,pitch;static int paused,frames,failures,actor_view;
static unsigned peak_strips,total_dropped;
static int sweep_left,sweep_frame;
static unsigned last_visible;
static VcpePage pages[]={{256,128,8,1,0,0,0,32768}};
static VcpeBundle bundles[]={{0,sizeof texture_data,0,1}};
static void wall(float x0,float z0,float x1,float z1,uint32_t color)
{
 float length=vcpe_dist(x0,z0,x1,z1);int n=(int)(length/2)+1;
 for(int i=0;i<n&&wall_count<64;i++){
  float a=(float)i/n,b=(float)(i+1)/n;
  walls[wall_count++]=(VcpeRayWall){x0+(x1-x0)*a,z0+(z1-z0)*a,x0+(x1-x0)*b,z0+(z1-z0)*b,0,3,0,color};
 }
}
static void doorway(float z,int open,uint32_t color)
{
 wall(-3,z,-1,z,color);wall(1,z,3,z,color);
 if(!open)wall(-1,z,1,z,0xff807263);
}
static void draw_scene(void)
{
 wall_count=strips_drawn=0;
 last_visible=(unsigned)vcpe_sector_connected(&sectors,player.cell);
 for(int r=0;r<3;r++)if(last_visible&(1u<<r)){
  const VcpeSectorCell *c=&cells[r];uint32_t color=r==0?0xffc4d5dd:r==1?0xff8096a6:0xffa2a98d;
  wall(-3,c->z0,-3,c->z1,color);wall(3,c->z0,3,c->z1,color);
  if(r==0)wall(-3,0,3,0,color);
  if(r==2)wall(-3,-24,3,-24,color);
 }
 if(last_visible&3)doorway(-8,1,0xffa9b7be);
 if(last_visible&6)doorway(-16,portals[1].open,0xff8f9ba5);
 VcpeRayView view={player.x,player.z,player.y+1.55f,yaw,pitch,64,.08f,25,480,272};
 int count=vcpe_raycast(&view,walls,(int)wall_count,4,columns,120);
 if(count<0)vcpe_fatal("Wall data error","Invalid wall-strip view");
 float focal=136*vcpe_cos_deg(32)/vcpe_sin_deg(32);
 float horizon=136+vcpe_sin_deg(pitch)/vcpe_cos_deg(pitch)*focal;
 vcpe_opaque(1);
 vcpe_fill(0,0,480,272,player.cell==0?0xffb9a589:0xff262b2d);
 if(horizon<272){float y=vcpe_clampf(horizon,0,272);vcpe_fill(0,y,480,272-y,0xff444c4b);}
 vcpe_bind_page(0);vcpe_filter(1);vcpe_tex_white(0);
 for(int i=0;i<count;i++){
  VcpeRayColumn *c=&columns[i];if(c->wall<0||c->bottom<=0||c->top>=272)continue;
  float y0=vcpe_clampf(c->top,0,272),y1=vcpe_clampf(c->bottom,0,272);
  float u=vcpe_clampf(c->tex_u*64,0,63),k=64/(c->bottom-c->top);
  vcpe_quad(i*4,y0,i*4+4,y1,u,(y0-c->top)*k,u+1,(y1-c->top)*k,walls[c->wall].color);strips_drawn++;
 }
 vcpe_opaque(0);
 /* Guard cutout uses only clipped 2D sprites, with one wall distance per strip. */
 const float ax=.9f,az=-5.7f;
 actor_view=vcpe_perspective_view(player.x,player.z,ax,az,0,8);
 float sn=vcpe_sin_deg(yaw),cs=vcpe_cos_deg(yaw),dx=ax-player.x,dz=az-player.z;
 float depth=-sn*dx-cs*dz,side=cs*dx-sn*dz;
 if((last_visible&1)&&depth>.1f){
  float x0=240+(side-.4f)*focal/depth,x1=240+(side+.4f)*focal/depth;
  float top=horizon+(view.eye-1.9f)*focal/depth,bottom=horizon+view.eye*focal/depth;
  float y0=vcpe_clampf(top,0,272),y1=vcpe_clampf(bottom,0,272);
  float ubase=128+(actor_view%4)*32,vbase=(actor_view/4)*64;
  int first=vcpe_clampi((int)(x0/4),0,119),last=vcpe_clampi((int)(x1/4),0,119);
  if(y1>y0)for(int i=first;i<=last;i++)if(depth<columns[i].depth){
   float a=vcpe_clampf(i*4,x0,x1),b=vcpe_clampf(i*4+4,x0,x1);if(b<=a)continue;
   vcpe_quad(a,y0,b,y1,ubase+(a-x0)*32/(x1-x0),vbase+(y0-top)*64/(bottom-top),
    ubase+(b-x0)*32/(x1-x0),vbase+(y1-top)*64/(bottom-top),0xffffffff);strips_drawn++;
  }
 }
 if(strips_drawn>peak_strips)peak_strips=strips_drawn;
 total_dropped+=(unsigned)vcpe_stat_dlist_full;
}
static void check(const char *label,int ok){printf("%s %s\n",ok?"PASS":"FAIL",label);if(!ok)failures++;}
static int command(const char *cmd,const char *arg,int n,int n2,const char *line,unsigned *held)
{
 if(!strcmp(cmd,"status")){
  printf("FLAT frame %d cell %d pos %.3f %.3f yaw %.1f view %d visible %u walls %u strips %u dropped %u scratch %u textures %u depth_bytes 0 failures %d peak_strips %u total_dropped %u\n",
   frames,player.cell,player.x,player.z,yaw,actor_view,last_visible,wall_count,strips_drawn,
   (unsigned)vcpe_stat_dlist_full,(unsigned)(sizeof columns+sizeof walls),(unsigned)sizeof texture_data,failures,peak_strips,total_dropped);
  printf("TIMING build_us %d gpu_wait_us %d draws %d dlist_kb %d cpu_mhz %d\n",
    vcpe_stat_build_us,vcpe_stat_gpu_wait_us,vcpe_stat_draws,vcpe_stat_dlist_kb,scePowerGetCpuClockFrequencyInt());
  return VCPE_CMD_NEXT;
 }
 if(!strcmp(cmd,"door")){portals[1].open=atoi(arg);return VCPE_CMD_NEXT;}
 if(!strcmp(cmd,"sweep")){sweep_left=vcpe_clampi(atoi(arg),0,10000);sweep_frame=0;return VCPE_CMD_FRAME;}
 if(!strcmp(cmd,"camera")){
  float x,z,a,p;if(sscanf(line,"%*s %f %f %f %f",&x,&z,&a,&p)==4){
   for(int i=0;i<3;i++)if(x>=cells[i].x0&&x<=cells[i].x1&&z>=cells[i].z0&&z<=cells[i].z1){
    player.x=x;player.z=z;player.cell=i;yaw=a;pitch=p;break;
   }
  }return VCPE_CMD_NEXT;
 }
 if(!strcmp(cmd,"expectcell")){check("cell transition",player.cell==atoi(arg));return VCPE_CMD_NEXT;}
 if(!strcmp(cmd,"expectblocked")){check("closed doorway collision",player.cell==1&&player.z> -16);return VCPE_CMD_NEXT;}
 if(!strcmp(cmd,"expectrender")){check("flat scene submitted without overflow",strips_drawn>0&&!total_dropped);return VCPE_CMD_NEXT;}
 return VCPE_CMD_UNKNOWN;
}
static int busy(unsigned *held)
{
 if(!sweep_left)return 0;
 sweep_left--;sweep_frame++;yaw=vcpe_angle_diff_deg(sweep_frame*2.7f,0);
 pitch=vcpe_sin_deg(sweep_frame*1.9f)*40;
 *held=0;return 1;
}
static void simulate(unsigned held,float dt)
{
 if(paused)return;
 int sx,sy;vcpe_pad_stick(&sx,&sy);
 float forward=(held&PSP_CTRL_UP?1:0)-(held&PSP_CTRL_DOWN?1:0);
 float strafe=(held&PSP_CTRL_RTRIGGER?1:0)-(held&PSP_CTRL_LTRIGGER?1:0);
 if(sy>25||sy< -25)forward-=sy/128.0f;
 if(sx>25||sx< -25)strafe+=sx/128.0f;
 yaw+=((held&PSP_CTRL_LEFT?1:0)-(held&PSP_CTRL_RIGHT?1:0))*90*dt;
 yaw=vcpe_angle_diff_deg(yaw,0);
 if(held&PSP_CTRL_TRIANGLE)pitch=vcpe_clampf(pitch+45*dt,-45,45);
 if(held&PSP_CTRL_SQUARE)pitch=vcpe_clampf(pitch-45*dt,-45,45);
 float len=vcpe_sqrt(forward*forward+strafe*strafe);if(len>1){forward/=len;strafe/=len;}
 float sn=vcpe_sin_deg(yaw),cs=vcpe_cos_deg(yaw),speed=2.8f*dt;
 vcpe_sector_move(&sectors,&player,(-sn*forward+cs*strafe)*speed,(-cs*forward-sn*strafe)*speed);
}
int main(int argc,char **argv)
{
 vcpe_sys_init(argc,argv);
 vcpe_log_open("FLAT.LOG");vcpe_log("LITE 1.03: sys ready, heap 4096 KB, stack 128 KB");
 vcpe_log("Starting graphics");if(vcpe_gfx_init()<0)return 1;vcpe_log("Graphics ready");
 vcpe_gfx_batch(1);vcpe_gfx_pipeline(0);
 vcpe_pad_init(0,0,VCPE_STICK_NONE);
 VcpeTestHooks hooks={vcpe_pad_button,command,0,busy,0};
 /* An accidentally copied AUTOTEST.TXT must never disable normal controls.
  * Diagnostics require a separate opt-in marker or both triggers at startup. */
 SceCtrlData boot_pad={0};sceCtrlPeekBufferPositive(&boot_pad,1);
 char marker[300];vcpe_path(marker,sizeof marker,"TESTMODE.ON");FILE *test_marker=fopen(marker,"rb");
 int test_requested=test_marker!=0 ||
     (boot_pad.Buttons&(PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER))==(PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER);
 if(test_marker)fclose(test_marker);
 if(test_requested)vcpe_test_load(&hooks);
 vcpe_log("Input mode: %s",vcpe_autotest?"AUTOTEST explicitly enabled":"MANUAL; AUTOTEST.TXT ignored");
 vcpe_loading_show_after(0);
 vcpe_loading_begin(sizeof texture_data);
 unsigned load_start=sceKernelGetSystemTimeLow();
 int st=vcpe_step_begin("Loading flat scene textures");
 vcpe_gfx_tables(pages,bundles,1);
 sceKernelDcacheWritebackRange(texture_data,sizeof texture_data);vcpe_bundle_attach(0,texture_data);
 vcpe_loading_add(sizeof texture_data);vcpe_step_end(st,1);
 st=vcpe_step_begin("Checking room connections");
 if(vcpe_sector_validate(&sectors)<0)vcpe_fatal("Room data error","Invalid sector connections");
 vcpe_step_end(st,1);
 /* Deliberate short boot preview for this hardware diagnostic. The loader's
  * normal 150 ms suppression otherwise hides these tiny linked assets. */
 int loader_captured=0;
 while((unsigned)(sceKernelGetSystemTimeLow()-load_start)<650000u){
  vcpe_loading_tick();
  if(vcpe_autotest&&!loader_captured && (unsigned)(sceKernelGetSystemTimeLow()-load_start)>300000u){
   vcpe_test_snap("loader");loader_captured=1;
  }
  sceKernelDelayThread(10000);
 }
 vcpe_loading_end();vcpe_log("VCPE loading screen complete: %d frames",vcpe_loading_shown());
 vcpe_log("Textures and cells ready; standard 2D strips, no GPU triangles, synchronous drawing");
 float accumulator=0;
 while(vcpe_running){
  vcpe_pad_update();unsigned pressed=vcpe_pad_pressed(),held=vcpe_pad_held();
  if(pressed&PSP_CTRL_START)paused=!paused;
  if((pressed&PSP_CTRL_CROSS)&&player.cell==1&&player.z< -13)portals[1].open=!portals[1].open;
  vcpe_perf_update_begin();
  accumulator+=vcpe_autotest?1/60.0f:vcpe_dt();
  int steps=0;while(accumulator>=1/60.0f && steps<2){simulate(held,1/60.0f);accumulator-=1/60.0f;steps++;}
  if(steps==2)accumulator=0;
  vcpe_perf_update_end();
  if(frames<2)vcpe_log("Frame %d begin",frames);
  vcpe_gfx_begin(0xffb9a589);
  draw_scene();
  if(frames<2)vcpe_log("Frame %d strips queued: %u",frames,strips_drawn);
  vcpe_fill(0,0,480,26,0xe81c1c1c);
  vcpe_hud_text(8,5,"VCPE / PERSPECTIVE LITE 1.03",0xffe1d8bd,1);
  vcpe_hud_text(8,16,vcpe_autotest?"AUTOTEST INPUT / 2D WALL STRIPS / SYNC":"MANUAL INPUT / 2D WALL STRIPS / SYNC",0xffb7b7ad,1);
  vcpe_hud_printf(400,16,0xffb7b7ad,"F%05u",(unsigned)frames%100000u);
  vcpe_fill(0,229,480,43,0xee181818);
  vcpe_hud_printf(8,234,0xffece1c7,"ROOM %d/3   %u STRIPS   %u KB ART   VIEW %d/8",player.cell+1,strips_drawn,(unsigned)(sizeof texture_data/1024),actor_view+1);
  vcpe_hud_text(8,246,"STICK MOVE  LEFT/RIGHT TURN  L/R STRAFE",0xffc0c0b6,1);
  vcpe_hud_text(8,258,player.cell==1?"X DOOR   TRI/SQ LOOK   START PAUSE":"TRI/SQ LOOK   START PAUSE   SELECT STATS",0xffc0c0b6,1);
  vcpe_fill(237,135,6,1,0xffdedede);vcpe_fill(240,132,1,7,0xffdedede);
  if(paused){vcpe_fill(180,118,120,28,0xef101010);vcpe_hud_text(216,128,"PAUSED",0xffffffff,1);}
  if(held&PSP_CTRL_SELECT){vcpe_hud_perf_detail(1);vcpe_hud_perf();}
  if(frames<2)vcpe_log("Frame %d submitting",frames);
  vcpe_gfx_end();vcpe_test_frame_end();
  if(frames<2||frames==120||frames%600==0)vcpe_log("Frame %d complete: %u strips, build %d us, GPU %d us, dropped %u",frames,strips_drawn,vcpe_stat_build_us,vcpe_stat_gpu_wait_us,total_dropped);
  frames++;
 }
 vcpe_gfx_present();vcpe_gfx_shutdown();vcpe_log_close();sceKernelExitGame();return failures?1:0;
}
