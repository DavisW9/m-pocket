#pragma once
#include <linux/input.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fstream>
static volatile sig_atomic_t stopRequested=0;
static void stopSignal(int){stopRequested=1;}
static std::string framebufferPath(){
    if(const char *path=getenv("M_POCKET_FB"))return path;
    std::ifstream list("/proc/fb");std::string line;
    while(std::getline(list,line))if(line.find("st7789")!=std::string::npos){auto end=line.find(' ');return "/dev/fb"+line.substr(0,end);}
    return "/dev/fb0";
}
static int openKeyboard(){
    if(const char *path=getenv("M_POCKET_INPUT"))return open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    DIR *dir=opendir("/dev/input");if(!dir)return -1;
    int fallback=-1,result=-1;
    while(auto *entry=readdir(dir)){
        if(strncmp(entry->d_name,"event",5))continue;
        std::string path=std::string("/dev/input/")+entry->d_name;
        int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)continue;
        char name[256]={};ioctl(fd,EVIOCGNAME(sizeof(name)),name);
        if(strstr(name,"tca8418")||strstr(name,"TCA8418")){result=fd;break;}
        unsigned char bits[(KEY_MAX+8)/8]={};
        if(ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(bits)),bits)>=0 && (bits[KEY_Q/8]&(1<<(KEY_Q%8))) && (bits[KEY_ENTER/8]&(1<<(KEY_ENTER%8)))){
            if(fallback<0){fallback=fd;continue;}
        }
        close(fd);
    }
    closedir(dir);if(result>=0){if(fallback>=0)close(fallback);return result;}return fallback;
}
#include "keyboard_text.hpp"
static bool leftShift=false,rightShift=false,caps=false,sym=false;
static unsigned heldCode=0;
static double repeatAt=0;
static void dispatchKey(unsigned code,bool repeated){
    int k=0;
    switch(code){case KEY_ESC:k=K_ESCAPE;break;case KEY_ENTER:k=K_ENTER;break;case KEY_BACKSPACE:k=K_BACKSPACE;break;
        case KEY_LEFT:k=K_LEFT;break;case KEY_RIGHT:k=K_RIGHT;break;case KEY_UP:k=K_UP;break;case KEY_DOWN:k=K_DOWN;break;}
    char text=typedCharacter(code,leftShift||rightShift,caps,sym);
    if(!k && text)k=(text>='A'&&text<='Z')?text+32:text;
    if(k)key(k,text?std::string(1,text):"",repeated);
}
static void readKeyboard(int fd){
    input_event event;
    for(int n=0;n<64 && read(fd,&event,sizeof(event))==(ssize_t)sizeof(event);n++){
        if(event.type==EV_SYN && event.code==SYN_DROPPED){heldCode=0;leftShift=rightShift=false;continue;}
        if(event.type!=EV_KEY)continue;
        if(event.code==KEY_LEFTSHIFT){leftShift=event.value!=0;continue;}
        if(event.code==KEY_RIGHTSHIFT){rightShift=event.value!=0;continue;}
        if(event.code==KEY_CAPSLOCK){if(event.value==1)caps=!caps;continue;}
        if(event.code==KEY_COMPOSE){if(event.value==1)sym=!sym;continue;}
        if(event.value==0){if(heldCode==event.code)heldCode=0;continue;}
        if(event.value!=1)continue; // Own repeat works with keyboards without EV_REP.
        dispatchKey(event.code,false);
        bool repeatable=event.code==KEY_X||event.code==KEY_F||event.code==KEY_UP||event.code==KEY_DOWN||event.code==KEY_BACKSPACE;
        heldCode=repeatable?event.code:0;repeatAt=nowSeconds()+0.4;
    }
    if(heldCode && nowSeconds()>=repeatAt){dispatchKey(heldCode,true);repeatAt=nowSeconds()+0.07;}
}
