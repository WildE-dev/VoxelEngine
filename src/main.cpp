#include <cstdlib>

#include "app/Application.h"

int main()
{
    Application app;
    if (!app.Init()) return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
