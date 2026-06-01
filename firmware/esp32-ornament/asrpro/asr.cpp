#include "asr.h"
extern "C"{ void * __dso_handle = 0 ;}
#include "setup.h"
#include "HardwareSerial.h"
#include "myLib/asr_event.h"
#include "FreeRTOS.h"
#include "task.h"

uint32_t snid;
void ASR_CODE();

//{speak:xiaodie,vol:2,speed:10,platform:haohaodada}
//{playid:10001,voice:welcome}
//{playid:10002,voice:}

static String serial0_rx;
static String serial1_rx;
static const char done_trigger[] = "codex_done";
static uint8_t serial0_match_index;
static uint8_t serial1_match_index;

typedef struct {
  uint32_t id;
  const char *token;
  const char *label;
} voice_command_t;

static const voice_command_t voice_commands[] = {
  {2001, "voice_status", "status"},
  {2002, "show_quota", "quota"},
  {2003, "show_tasks", "tasks"},
  {2004, "show_clock", "clock"},
  {2005, "refresh_state", "refresh"},
  {2006, "bridge_match", "bridge_match"},
  {2007, "quiet_on", "quiet_on"},
  {2008, "quiet_off", "quiet_off"},
};

static void trim_rx_buffer(String &rx)
{
  if(rx.length() > 80){
    rx = rx.substring(rx.length() - 32);
  }
}

static bool update_done_match(char ch, uint8_t &match_index)
{
  if(ch == done_trigger[match_index]){
    match_index++;
    if(done_trigger[match_index] == '\0'){
      match_index = 0;
      return true;
    }
    return false;
  }

  match_index = (ch == done_trigger[0]) ? 1 : 0;
  return false;
}

static void notify_done_match(const char *label)
{
  Serial.print(label);
  Serial.println(" matched codex_done, play 10500");
  //{playid:10500,voice:task done}
  play_audio(10500);
}

static void poll_serial_port(HardwareSerial &port, String &rx, uint8_t &match_index, const char *label)
{
  bool got_data = false;
  while(port.available()){
    char ch = (char)port.read();
    rx += ch;
    if(update_done_match(ch, match_index)){
      notify_done_match(label);
      rx = "";
    }
    got_data = true;
  }

  if(!got_data){
    return;
  }

  Serial.print(label);
  Serial.print(" rx: ");
  Serial.println(rx);
  trim_rx_buffer(rx);
}

static void serial_done_task(void *arg)
{
  (void)arg;
  while(true){
    set_wakeup_forever();
    poll_serial_port(Serial1, serial1_rx, serial1_match_index, "Serial1");
    poll_serial_port(Serial, serial0_rx, serial0_match_index, "Serial0");
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static bool send_voice_command(uint32_t id)
{
  const size_t count = sizeof(voice_commands) / sizeof(voice_commands[0]);
  for(size_t i = 0; i < count; i++){
    if(voice_commands[i].id == id){
      Serial.print("voice command ");
      Serial.print(voice_commands[i].label);
      Serial.print(" -> ");
      Serial.println(voice_commands[i].token);
      Serial1.print(voice_commands[i].token);
      Serial1.print("\r\n");
      return true;
    }
  }
  return false;
}

void ASR_CODE(){
  set_wakeup_forever();
  if(!send_voice_command(snid)){
    Serial.print("unhandled snid: ");
    Serial.println(snid);
  }
}

void hardware_init(){
  vol_set(1);
  xTaskCreate(serial_done_task, "serial_done", 1024, NULL, 4, NULL);
  vTaskDelete(NULL);
}

void setup()
{
  setPinFun(2,FORTH_FUNCTION);
  setPinFun(3,FORTH_FUNCTION);
  Serial.begin(115200);
  Serial.println("ASRPRO debug boot");
  Serial1.begin(9600);
  Serial.println("Serial1 begin 9600, waiting codex_done and sending voice commands");
  //{playid:0,voice:task done}
  //{ID:8,keyword:"wake",ASR:"ailisi",ASRTO:"ok"}
  //{ID:2001,keyword:"cmd",ASR:"dangqianzhuangtai",ASRTO:"ok"}
  //{ID:2002,keyword:"cmd",ASR:"xianshiedu",ASRTO:"ok"}
  //{ID:2003,keyword:"cmd",ASR:"xianshirenwu",ASRTO:"ok"}
  //{ID:2004,keyword:"cmd",ASR:"xianshishizhong",ASRTO:"ok"}
  //{ID:2005,keyword:"cmd",ASR:"shuaxinyixia",ASRTO:"ok"}
  //{ID:2006,keyword:"cmd",ASR:"chongxinpipei",ASRTO:"ok"}
  //{ID:2007,keyword:"cmd",ASR:"anjingmoshi",ASRTO:"ok"}
  //{ID:2008,keyword:"cmd",ASR:"huifutixing",ASRTO:"ok"}
}
