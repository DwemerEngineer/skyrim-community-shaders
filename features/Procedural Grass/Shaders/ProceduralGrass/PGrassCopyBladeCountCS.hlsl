RWByteAddressBuffer args : register(u1);

#if defined(HIGH_VERTEX)
#define INDICES_PER_BLADE 39
#else // LOW_VERTEX
#define INDICES_PER_BLADE 15
#endif

[numthreads(1, 1, 1)] void main() {
		
	args.Store(0, INDICES_PER_BLADE);
	uint instanceCount = args.Load(4);
	args.Store(4, instanceCount);
	args.Store(8, 0);
	args.Store(12, 0);
	args.Store(16, 0);
}
