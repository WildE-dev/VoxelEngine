#include "VoxelEngine.h"

int main(int argc, char* argv[]) {
	VoxelEngine engine = VoxelEngine();
	engine.Initialize();
	engine.Start();
	engine.Shutdown();
	return 0;
}