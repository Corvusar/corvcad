/* CorvCAD mechanical session boundary. MIT. No FreeCAD/Python types cross it. */
#pragma once
#include "c_api.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cc_mechanical_session cc_mechanical_session;
/* Default builds return 0. The native edition links the reviewed F3 libraries. */
int cc_mechanical_available(void);
/* Initialize once with read-only bundled runtime and writable app-private data.
 * All operations are serialized by the implementation. The runtime must outlive
 * all sessions; Python/FreeCAD are process-scoped and are never finalized here. */
int cc_mechanical_initialize(const char *runtime_root, const char *user_root,
                             char *error, size_t error_size);
/* Open an existing enclosure.FCStd or create an unsaved model at that path.
 * input_json uses the validated F1 board schema, not executable Python.
 * Existing document parameters take precedence over initial_settings.
 * Existing documents must match board provenance; open never overwrites data. */
cc_mechanical_session *cc_mechanical_open(const char *document_path, const char *input_json,
                                         cc_settings initial_settings, char *error, size_t error_size);
void cc_mechanical_close(cc_mechanical_session *session);
int cc_mechanical_get_settings(cc_mechanical_session *session, cc_settings *settings,
                               char *error, size_t error_size);
/* A validated parameter change recomputes atomically and creates one undo step.
 * Invalid changes preserve the document and history. Names match cc_settings. */
int cc_mechanical_set_parameter(cc_mechanical_session *session, const char *name, double value,
                                char *error, size_t error_size);
int cc_mechanical_recompute(cc_mechanical_session *session, char *error, size_t error_size);
int cc_mechanical_can_undo(cc_mechanical_session *session);
int cc_mechanical_can_redo(cc_mechanical_session *session);
int cc_mechanical_undo(cc_mechanical_session *session, char *error, size_t error_size);
int cc_mechanical_redo(cc_mechanical_session *session, char *error, size_t error_size);
/* part: 0 tray, 1 lid. Returns required count; a short buffer is not written.
 * Preview and exports derive from the same exact solids. Invalid input returns
 * zero and writes an error. Output remains owned by the caller. */
size_t cc_mechanical_triangles(cc_mechanical_session *session, int part,
                               cc_triangle *output, size_t capacity, char *error, size_t error_size);
/* Explicit saves and exports refuse external modifications/unknown overwrite.
 * STEP is an exact-solid export; app availability remains gated through F5. */
int cc_mechanical_save(cc_mechanical_session *session, char *error, size_t error_size);
int cc_mechanical_export_step(cc_mechanical_session *session, int part, const char *path,
                              char *error, size_t error_size);
int cc_mechanical_export_stl(cc_mechanical_session *session, int part, const char *path,
                             char *error, size_t error_size);
/* The session's JSON requests, by method name with a JSON object of arguments (NULL: none).
 *   "features"  {} -> {"features": [{"body", "name", "type", "label", "suppressed",
 *                                    "profile", "expressions": {property: expression}}]}
 *               the PartDesign features of the Tray and Lid bodies, in tree order.
 * Returns JSON text (free with cc_free_text), or NULL with the error. */
char *cc_mechanical_call_json(cc_mechanical_session *session, const char *method, const char *arguments,
                              char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
