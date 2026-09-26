#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct Vec3 { float x=0, y=0, z=0; };
struct Aabb { Vec3 min, max; };
struct Plane { float a=0,b=0,c=0,d=0; };

enum class Relation { Outside, Intersect, Inside };

struct Frustum {
    std::array<Plane,6> planes;
    // The matrix is row-major; vectors multiply on the left: clip = world * viewProjection.
    static Frustum fromMatrix(const float m[4][4]) {
        auto make = [](float a,float b,float c,float d) {
            const float length = std::sqrt(a*a+b*b+c*c);
            return Plane{a/length,b/length,c/length,d/length};
        };
        Frustum f;
        f.planes[0]=make(m[0][3]+m[0][0],m[1][3]+m[1][0],m[2][3]+m[2][0],m[3][3]+m[3][0]); // left
        f.planes[1]=make(m[0][3]-m[0][0],m[1][3]-m[1][0],m[2][3]-m[2][0],m[3][3]-m[3][0]); // right
        f.planes[2]=make(m[0][3]+m[0][1],m[1][3]+m[1][1],m[2][3]+m[2][1],m[3][3]+m[3][1]); // bottom
        f.planes[3]=make(m[0][3]-m[0][1],m[1][3]-m[1][1],m[2][3]-m[2][1],m[3][3]-m[3][1]); // top
        f.planes[4]=make(m[0][2],m[1][2],m[2][2],m[3][2]); // D3D near: z >= 0
        f.planes[5]=make(m[0][3]-m[0][2],m[1][3]-m[1][2],m[2][3]-m[2][2],m[3][3]-m[3][2]); // far
        return f;
    }
    Relation classify(const Aabb& box) const {
        bool intersects=false;
        for (const Plane& p:planes) {
            const Vec3 positive{p.a>=0?box.max.x:box.min.x,p.b>=0?box.max.y:box.min.y,p.c>=0?box.max.z:box.min.z};
            const Vec3 negative{p.a>=0?box.min.x:box.max.x,p.b>=0?box.min.y:box.max.y,p.c>=0?box.min.z:box.max.z};
            const auto distance=[&](Vec3 v){return p.a*v.x+p.b*v.y+p.c*v.z+p.d;};
            if(distance(positive)<0) return Relation::Outside;
            if(distance(negative)<0) intersects=true;
        }
        return intersects?Relation::Intersect:Relation::Inside;
    }
};

inline bool contains(const Aabb& outer,const Aabb& inner) {
    return inner.min.x>=outer.min.x && inner.min.y>=outer.min.y && inner.min.z>=outer.min.z &&
           inner.max.x<=outer.max.x && inner.max.y<=outer.max.y && inner.max.z<=outer.max.z;
}

struct CullStats { uint32_t nodesTested=0, objectsTested=0; };

class Octree {
    struct Node {
        Aabb bounds;
        std::vector<uint32_t> objects;
        std::array<int,8> children{};
        Node(Aabb b):bounds(b){children.fill(-1);}
    };
    std::vector<Node> nodes_;
    const std::vector<Aabb>* boxes_=nullptr;
    int maxDepth_=7;
    uint32_t leafSize_=20;

    int makeNode(const Aabb& bounds,std::vector<uint32_t> ids,int depth) {
        const int index=static_cast<int>(nodes_.size());
        nodes_.emplace_back(bounds);
        if(depth>=maxDepth_ || ids.size()<=leafSize_) {nodes_[index].objects=std::move(ids);return index;}
        const Vec3 mid{(bounds.min.x+bounds.max.x)*0.5f,(bounds.min.y+bounds.max.y)*0.5f,(bounds.min.z+bounds.max.z)*0.5f};
        std::array<std::vector<uint32_t>,8> childIds;
        for(uint32_t id:ids) {
            const Aabb& b=(*boxes_)[id];
            const Vec3 center{(b.min.x+b.max.x)*0.5f,(b.min.y+b.max.y)*0.5f,(b.min.z+b.max.z)*0.5f};
            const int slot=(center.x>=mid.x?1:0)|(center.y>=mid.y?2:0)|(center.z>=mid.z?4:0);
            Aabb child{{slot&1?mid.x:bounds.min.x,slot&2?mid.y:bounds.min.y,slot&4?mid.z:bounds.min.z},
                       {slot&1?bounds.max.x:mid.x,slot&2?bounds.max.y:mid.y,slot&4?bounds.max.z:mid.z}};
            if(contains(child,b)) childIds[slot].push_back(id);
            else nodes_[index].objects.push_back(id); // straddlers stay in the parent
        }
        for(int slot=0;slot<8;++slot) if(!childIds[slot].empty()) {
            Aabb child{{slot&1?mid.x:bounds.min.x,slot&2?mid.y:bounds.min.y,slot&4?mid.z:bounds.min.z},
                       {slot&1?bounds.max.x:mid.x,slot&2?bounds.max.y:mid.y,slot&4?bounds.max.z:mid.z}};
            const int childIndex=makeNode(child,std::move(childIds[slot]),depth+1);
            nodes_[index].children[slot]=childIndex;
        }
        return index;
    }
    void appendAll(int index,std::vector<uint32_t>& out) const {
        const Node& n=nodes_[index];
        out.insert(out.end(),n.objects.begin(),n.objects.end());
        for(int child:n.children) if(child>=0) appendAll(child,out);
    }
    void traverse(int index,const Frustum& frustum,std::vector<uint32_t>& out,CullStats& stats) const {
        ++stats.nodesTested;
        const Node& n=nodes_[index];
        const Relation relation=frustum.classify(n.bounds);
        if(relation==Relation::Outside) return;
        if(relation==Relation::Inside) {appendAll(index,out);return;}
        for(uint32_t id:n.objects) {
            ++stats.objectsTested;
            if(frustum.classify((*boxes_)[id])!=Relation::Outside) out.push_back(id);
        }
        for(int child:n.children) if(child>=0) traverse(child,frustum,out,stats);
    }
public:
    void build(const std::vector<Aabb>& boxes) {
        boxes_=&boxes;nodes_.clear();
        if(boxes.empty()) return;
        Aabb root=boxes.front();
        for(const Aabb& b:boxes) {
            root.min.x=std::min(root.min.x,b.min.x);root.min.y=std::min(root.min.y,b.min.y);root.min.z=std::min(root.min.z,b.min.z);
            root.max.x=std::max(root.max.x,b.max.x);root.max.y=std::max(root.max.y,b.max.y);root.max.z=std::max(root.max.z,b.max.z);
        }
        // A flat ground scene has nearly identical Y intervals. Give the root
        // enough Y extent so the first split plane does not cut every object.
        const float span=std::max({root.max.x-root.min.x,root.max.z-root.min.z,root.max.y-root.min.y});
        root.max.y=root.min.y+2.f*span;
        std::vector<uint32_t> ids(boxes.size());
        for(uint32_t i=0;i<ids.size();++i) ids[i]=i;
        makeNode(root,std::move(ids),0);
    }
    void visible(const Frustum& frustum,std::vector<uint32_t>& out,CullStats& stats) const {
        out.clear();stats={};
        if(!nodes_.empty()) traverse(0,frustum,out,stats);
    }
    size_t nodeCount() const {return nodes_.size();}
};
