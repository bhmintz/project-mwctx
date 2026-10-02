#pragma once

// See nfsmw_hilos_android.cpp: the ring thread's priority and who takes its core in a stutter (Android only;
// both are no-ops elsewhere).
namespace nfsmw::hilos {
void AlPresentar();             // from the ring thread, every Swap
void AnotarTiron(double ms);    // from the ring thread, when a frame took ms
}  // namespace nfsmw::hilos
