#include "corvcad/schematic_session_c_api.h"
#include "corvcad/c_api.h"
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: corvcad_schematic_session_cli project.kicad_pro\n";
    return 2;
  }
  cc_sch_result result{};
  cc_sch_session *session = cc_sch_session_open(argv[1], &result);
  if (!session) {
    std::cerr << (result.message ? result.message : "Could not open schematic") << '\n';
    cc_sch_result_clear(&result);
    return 1;
  }
  char *json = cc_sch_session_snapshot_json(session, &result);
  if (!json) {
    std::cerr << (result.message ? result.message : "Could not inspect schematic") << '\n';
    cc_sch_session_free(session);
    cc_sch_result_clear(&result);
    return 1;
  }
  std::cout << json << '\n';
  cc_free_text(json);
  cc_sch_session_free(session);
  cc_sch_result_clear(&result);
  return 0;
}
