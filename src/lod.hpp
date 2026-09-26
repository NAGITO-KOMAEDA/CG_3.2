#pragma once
#include "culling.hpp"
enum class Lod { Model=0, Billboard=1, Hidden=2 };
inline Lod selectLod(Vec3 eye, Vec3 center, float modelDistance, float hideDistance, int forced=-1) {
    if(forced>=0 && forced<=2)return static_cast<Lod>(forced);
    const float x=eye.x-center.x,y=eye.y-center.y,z=eye.z-center.z;
    const float d2=x*x+y*y+z*z;
    if(d2<modelDistance*modelDistance)return Lod::Model;
    if(d2<hideDistance*hideDistance)return Lod::Billboard;
    return Lod::Hidden;
}
