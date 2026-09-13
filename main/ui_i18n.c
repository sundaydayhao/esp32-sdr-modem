#include "ui_i18n.h"
#include <stdarg.h>
#include <string.h>
#include "ui_strings.inc"
const char *ui_tr(const char *text)
{
    if(!text)return "";
    for(size_t n=0;n<sizeof(ui_strings)/sizeof(ui_strings[0]);++n)
        if(!strcmp(text,ui_strings[n][0]))return ui_strings[n][1];
    return text;
}
void ui_label_set_text(lv_obj_t *obj,const char *text){lv_label_set_text(obj,ui_tr(text));}
void ui_label_set_text_fmt(lv_obj_t *obj,const char *format,...)
{
    va_list args;va_start(args,format);lv_label_set_text_vfmt(obj,ui_tr(format),args);va_end(args);
}
void ui_dropdown_set_options(lv_obj_t *obj,const char *options){lv_dropdown_set_options(obj,ui_tr(options));}
void ui_checkbox_set_text(lv_obj_t *obj,const char *text){lv_checkbox_set_text(obj,ui_tr(text));}
