#include "VoxelEngine.h"

int main() {
	VoxelEngine engine = VoxelEngine();
	engine.Initialize();
	engine.Start();
	engine.Shutdown();
	return 0;
}