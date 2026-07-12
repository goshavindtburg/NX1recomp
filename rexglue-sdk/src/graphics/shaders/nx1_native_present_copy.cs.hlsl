/**
 ******************************************************************************
 * ReXGlue NX1 native-present experiment
 ******************************************************************************
 */

cbuffer XeNativePresentConstants : register(b0) {
  uint2 xe_native_present_size;
};

Texture2D<float4> xe_native_present_source : register(t0);
RWTexture2D<float4> xe_native_present_dest : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  if (xe_thread_id.x >= xe_native_present_size.x ||
      xe_thread_id.y >= xe_native_present_size.y) {
    return;
  }

  xe_native_present_dest[xe_thread_id.xy] =
      xe_native_present_source.Load(int3(xe_thread_id.xy, 0));
}
