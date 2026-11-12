// Dependency-free edition of the optional mechanical adapter. MIT.
#include "corvcad/mechanical_session_c_api.h"
#include <algorithm>
#include <cstring>
namespace {
int unavailable(char *error, size_t size) noexcept {
  constexpr char message[] = "FreeCAD mechanical sessions are unavailable in this edition";
  if (error && size) {
    const size_t count = std::min(size - 1, sizeof(message) - 1);
    std::memcpy(error, message, count); error[count] = 0;
  }
  return 0;
}
}
extern "C" {
int cc_mechanical_available(void) { return 0; }
int cc_mechanical_initialize(const char *, const char *, char *e, size_t n) { return unavailable(e,n); }
cc_mechanical_session *cc_mechanical_open(const char *,const char *,cc_settings,char *e,size_t n) {
  unavailable(e,n); return nullptr;
}
void cc_mechanical_close(cc_mechanical_session *) {}
int cc_mechanical_get_settings(cc_mechanical_session *,cc_settings *,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_set_parameter(cc_mechanical_session *,const char *,double,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_recompute(cc_mechanical_session *,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_can_undo(cc_mechanical_session *) { return 0; }
int cc_mechanical_can_redo(cc_mechanical_session *) { return 0; }
int cc_mechanical_undo(cc_mechanical_session *,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_redo(cc_mechanical_session *,char *e,size_t n) { return unavailable(e,n); }
size_t cc_mechanical_triangles(cc_mechanical_session *,int,cc_triangle *,size_t,char *e,size_t n) {
  unavailable(e,n); return 0;
}
int cc_mechanical_save(cc_mechanical_session *,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_export_step(cc_mechanical_session *,int,const char *,char *e,size_t n) { return unavailable(e,n); }
int cc_mechanical_export_stl(cc_mechanical_session *,int,const char *,char *e,size_t n) { return unavailable(e,n); }
char *cc_mechanical_call_json(cc_mechanical_session *,const char *,const char *,char *e,size_t n) {
  unavailable(e,n); return nullptr;
}
}
