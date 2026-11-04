// Regular polyhedra with flat (geodesic) faces and the regular honeycombs (space-filling tilings)
// they make in flat, hyperbolic and spherical 3D space.
//
// A polyhedron is the intersection of half-spaces bounded by planes at distance h (the inradius)
// from its centre, perpendicular to its face directions d_i. In the curved model a plane at
// distance h along d is { p : mdot(p, n) = 0 } with n = (d cosK(h), -k sinK(h))
// (model units), and the signed distance of p to it is R asinK(mdot(p, n)): exact, cheap.
//
// A regular honeycomb {p,q,r} is made of copies of one cell (all mirror images across faces).
// Its dihedral angle t (360 / r degrees) fixes the cell size in curved space:
//     cosK(h)^2 = (1 + cos t) / (1 - c)      (c = cos of the angle between adjacent face directions)
// so changing the curvature radius scales the cells. Rendering folds a point into the central
// cell by reflecting it across the faces it lies outside of (each a symmetry of the honeycomb).
#pragma once
#include <string>
#include <vector>

#include "space.hpp"

namespace fd {

enum PolyType { POLY_TETRA = 0, POLY_CUBE, POLY_OCTA, POLY_DODECA, POLY_ICOSA, POLY_COUNT };
inline const char* POLY_NAMES[POLY_COUNT] = {"Tetrahedron", "Cube", "Octahedron", "Dodecahedron", "Icosahedron"};

/** Unit face directions (x, y, z, 0). */
const std::vector<V4>& polyFaces(int type);
/** Pairs of adjacent faces (one per edge). */
const std::vector<std::pair<int, int>>& polyEdges(int type);
/** cos of the angle between adjacent face directions. */
double polyAdjCos(int type);
/** Flat circumradius / inradius. */
double polyCircumRatio(int type);
/** Flat dihedral angle (radians). */
double polyFlatDihedral(int type);

struct Honeycomb {
  const char* name;       // e.g. "Order-5 cubic"
  const char* schlafli;   // {4,3,5}
  SpaceKind space;
  int cell;               // PolyType
  double dihedralDeg;     // angle between the faces at an edge (360 / cells around it)
  const char* note;
};
const std::vector<Honeycomb>& honeycombs();
/** Index of the first honeycomb of a space (-1: none). */
int firstHoneycomb(SpaceKind s);
/** Cell inradius in model units (multiply by R); flat honeycombs: 0 (sized by the object). */
double honeycombInradius(const Honeycomb& h);

/** Polyhedron / honeycomb parameters of a leaf (prim "polyhedron" / "honeycomb"). */
struct PolyParams {
  int type = POLY_CUBE;  // polyhedron
  int honey = 0;         // honeycomb (index into honeycombs())
  double size = 1;       // polyhedron inradius; flat honeycomb cell inradius
  int style = 0;         // honeycomb: 0 = edges, 1 = cells
  double width = 0.05;   // edge radius / gap between cells
  bool alternate = true; // honeycomb: neighbouring cells alternate the two colours
  bool operator==(const PolyParams& o) const {
    return type == o.type && honey == o.honey && size == o.size && style == o.style && width == o.width && alternate == o.alternate;
  }
};

// ---- distances (CPU; the shader code is generated from the same tables in codegen.cpp) ----
/** Flat polyhedron of inradius h, at local point u. */
double polyDistFlat(int type, double h, const V4& u);
/** Curved polyhedron of inradius hWorld, at a model point in its own frame. */
double polyDistCurved(const Space& s, int type, double hWorld, const V4& q);
/** The symmetry of a curved honeycomb (product of face reflections) that brings q into the
 *  central cell; parity = number of reflections mod 2. */
M4 honeycombCellOf(const Space& s, const PolyParams& pp, V4 q, int* parity);
/** Honeycomb at q (curved: model point in its frame; flat: local point). parity: cell colour. */
double honeycombDist(const Space& s, const PolyParams& pp, V4 q, int* parity = nullptr);

}  // namespace fd
