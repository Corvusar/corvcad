// CorvCAD static FreeCAD module registration, MIT.
// The entry points belong to the separately licensed FreeCAD libraries.
#include <Python.h>

extern "C" {
PyObject* PyInit_Materials();
PyObject* PyInit_Part();
PyObject* PyInit_Mesh();
PyObject* PyInit_Sketcher();
PyObject* PyInit__PartDesign();
PyObject* PyInit_Spreadsheet();
PyObject* PyInit_Import();
}

bool corvcad_register_freecad_modules()
{
    // FreeCAD's Application::initConfig appends FreeCAD/__FreeCADBase__ itself.
    // This function must run before that initialization, on the embedding thread.
    if (Py_IsInitialized()) return false;
    static struct _inittab modules[] = {
        {"Materials", PyInit_Materials},
        {"Part", PyInit_Part},
        {"Mesh", PyInit_Mesh},
        {"Sketcher", PyInit_Sketcher},
        {"_PartDesign", PyInit__PartDesign},
        {"Spreadsheet", PyInit_Spreadsheet},
        {"Import", PyInit_Import},
        {nullptr, nullptr}
    };
    return PyImport_ExtendInittab(modules) == 0;
}
