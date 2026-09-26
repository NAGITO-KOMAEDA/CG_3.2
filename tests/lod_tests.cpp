#include "lod.hpp"
#include <iostream>
#include <stdexcept>
static void expect(bool ok) { if(!ok)throw std::runtime_error("LOD test failed"); }
int main() {
    const Vec3 origin{};
    expect(selectLod(origin,{0,0,0},30,85)==Lod::Model);
    expect(selectLod(origin,{29.99f,0,0},30,85)==Lod::Model);
    expect(selectLod(origin,{30,0,0},30,85)==Lod::Billboard);
    expect(selectLod(origin,{0,0,-84.99f},30,85)==Lod::Billboard);
    expect(selectLod(origin,{0,85,0},30,85)==Lod::Hidden);
    expect(selectLod({10,20,30},{10,20,60},30,85)==Lod::Billboard);
    expect(selectLod(origin,{18,24,0},30,85)==Lod::Billboard);
    expect(selectLod(origin,{60,0,0},60,150)==Lod::Billboard);
    expect(selectLod(origin,{150,0,0},60,150)==Lod::Hidden);
    for(int mode=0;mode<3;++mode) {
        expect(selectLod(origin,origin,30,85,mode)==static_cast<Lod>(mode));
        expect(selectLod(origin,{1000,0,0},30,85,mode)==static_cast<Lod>(mode));
    }
    std::cout<<"LOD boundary, 3D distance and forced-mode tests passed\n";
}
