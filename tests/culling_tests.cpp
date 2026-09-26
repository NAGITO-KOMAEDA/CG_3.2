#include "culling.hpp"
#include <cassert>
#include <algorithm>
#include <iostream>

int main() {
    // D3D left-handed perspective, 90 degree FOV, aspect 1, near 1, far 10.
    const float m[4][4]={{1,0,0,0},{0,1,0,0},{0,0,10.f/9.f,1},{0,0,-10.f/9.f,0}};
    const Frustum f=Frustum::fromMatrix(m);
    assert(f.classify({{-0.1f,-0.1f,2},{0.1f,0.1f,2.2f}})==Relation::Inside);
    assert(f.classify({{20,0,2},{21,1,3}})==Relation::Outside);
    assert(f.classify({{0,0,0},{0.2f,0.2f,1.2f}})==Relation::Intersect);
    assert(f.classify({{0,0,11},{1,1,12}})==Relation::Outside);
    std::vector<Aabb> boxes;
    for(int z=-15;z<=15;++z) for(int x=-15;x<=15;++x)
        boxes.push_back({{x*2.f-.2f,-.2f,z*2.f-.2f},{x*2.f+.2f,.2f,z*2.f+.2f}});
    Octree tree;tree.build(boxes);
    assert(tree.nodeCount()>1);
    std::vector<uint32_t> actual,expected;
    CullStats stats;tree.visible(f,actual,stats);
    for(uint32_t i=0;i<boxes.size();++i) if(f.classify(boxes[i])!=Relation::Outside) expected.push_back(i);
    std::sort(actual.begin(),actual.end());
    assert(actual==expected);
    assert(stats.objectsTested<boxes.size());
    std::cout<<"frustum and octree tests passed; nodes="<<tree.nodeCount()<<" tested="<<stats.objectsTested<<"/"<<boxes.size()<<"\n";
}
