// 3D World - Building Interior Generation
// by Frank Gennari 11/15/19

#include "3DWorld.h"
#include "function_registry.h"
#include "buildings.h"
#include "lightmap.h" // for light_source

float const FLOOR_THICK_VAL = 0.1; // 10% of floor spacing

extern int display_mode;
extern building_params_t global_building_params;
extern vector<light_source> dl_sources;


void building_t::set_z_range(float z1, float z2) {
	bcube.z1() = z1; bcube.z2() = z2;
	adjust_part_zvals_for_floor_spacing(bcube);
	if (!parts.empty()) {parts[0].z1() = z1; parts[0].z2() = z2;}
}
building_mat_t const &building_t::get_material() const {return global_building_params.get_material(mat_ix);}

void building_t::gen_rotation(rand_gen_t &rgen) {

	float const max_rot_angle(get_material().max_rot_angle);
	if (max_rot_angle == 0.0) return;
	float const rot_angle(rgen.rand_uniform(0.0, max_rot_angle));
	rot_sin = sin(rot_angle);
	rot_cos = cos(rot_angle);
	parts.clear();
	parts.push_back(bcube); // this is the actual building base
	cube_t const &bc(parts.back());
	point const center(bc.get_cube_center());

	for (unsigned i = 0; i < 4; ++i) {
		point corner(bc.d[0][i&1], bc.d[1][i>>1], bc.d[2][i&1]);
		do_xy_rotate(rot_sin, rot_cos, center, corner);
		if (i == 0) {bcube.set_from_point(corner);} else {bcube.union_with_pt(corner);} // Note: detail cubes are excluded
	}
}

bool building_t::check_bcube_overlap_xy(building_t const &b, float expand_rel, float expand_abs, vector<point> &points) const {

	if (expand_rel == 0.0 && expand_abs == 0.0 && !bcube.intersects(b.bcube)) return 0;
	if (!is_rotated() && !b.is_rotated()) return 1; // above check is exact, top-level bcube check up to the caller
	if (b.bcube.contains_pt_xy(bcube.get_cube_center()) || bcube.contains_pt_xy(b.bcube.get_cube_center())) return 1; // slightly faster to include this check
	return (check_bcube_overlap_xy_one_dir(b, expand_rel, expand_abs, points) || b.check_bcube_overlap_xy_one_dir(*this, expand_rel, expand_abs, points));
}

// Note: only checks for point (x,y) value contained in one cube/N-gon/cylinder; assumes pt has already been rotated into local coordinate frame
bool building_t::check_part_contains_pt_xy(cube_t const &part, point const &pt, vector<point> &points) const {

	if (!part.contains_pt_xy(pt)) return 0; // check bounding cube
	if (is_simple_cube()) return 1; // that's it
	building_draw_utils::calc_poly_pts(*this, part, points);
	return point_in_polygon_2d(pt.x, pt.y, points.data(), points.size(), 0, 1); // 2D x/y containment
}

bool building_t::check_bcube_overlap_xy_one_dir(building_t const &b, float expand_rel, float expand_abs, vector<point> &points) const { // can be called before levels/splits are created

	// Note: easy cases are handled by check_bcube_overlap_xy() above
	point const center1(b.bcube.get_cube_center()), center2(bcube.get_cube_center());

	for (auto p1 = b.parts.begin(); p1 != b.parts.end(); ++p1) {
		point pts[9]; // {center, 00, 10, 01, 11, x0, x1, y0, y1}

		if (b.parts.size() == 1) {pts[0] = center1;} // single cube: we know we're rotating about its center
		else {
			pts[0] = p1->get_cube_center();
			do_xy_rotate(b.rot_sin, b.rot_cos, center1, pts[0]); // rotate into global space
		}
		cube_t c_exp(*p1);
		c_exp.expand_by_xy(expand_rel*p1->get_size() + vector3d(expand_abs, expand_abs, expand_abs));

		for (unsigned i = 0; i < 4; ++i) { // {00, 10, 01, 11}
			pts[i+1].assign(c_exp.d[0][i&1], c_exp.d[1][i>>1], 0.0); // XY only
			do_xy_rotate(b.rot_sin, b.rot_cos, center1, pts[i+1]); // rotate into global space
		}
		for (unsigned i = 0; i < 5; ++i) {do_xy_rotate(-rot_sin, rot_cos, center2, pts[i]);} // inverse rotate into local coord space - negate the sine term
		cube_t c_exp_rot(pts+1, 4); // use points 1-4
		pts[5] = 0.5*(pts[1] + pts[3]); // x0 edge center
		pts[6] = 0.5*(pts[2] + pts[4]); // x1 edge center
		pts[7] = 0.5*(pts[1] + pts[2]); // y0 edge center
		pts[8] = 0.5*(pts[3] + pts[4]); // y1 edge center

		for (auto p2 = parts.begin(); p2 != parts.end(); ++p2) {
			if (c_exp_rot.contains_pt_xy(p2->get_cube_center())) return 1; // quick and easy test for heavy overlap

			for (unsigned i = 0; i < 9; ++i) {
				if (check_part_contains_pt_xy(*p2, pts[i], points)) return 1; // Note: building geometry is likely not yet generated, this check should be sufficient
				//if (p2->contains_pt_xy(pts[i])) return 1;
			}
		}
	} // for p1
	return 0;
}

bool building_t::test_coll_with_sides(point &pos, point const &p_last, float radius, cube_t const &part, vector<point> &points, vector3d *cnorm) const {

	building_draw_utils::calc_poly_pts(*this, part, points); // without the expand
	point quad_pts[4]; // quads
	bool updated(0);

	// FIXME: if the player is moving too quickly, the intersection with a side polygon may be missed,
	// which allows the player to travel through the building, but using a line intersection test from p_past2 to pos has other problems
	for (unsigned S = 0; S < num_sides; ++S) { // generate vertex data quads
		for (unsigned d = 0, ix = 0; d < 2; ++d) {
			point const &p(points[(S+d)%num_sides]);
			for (unsigned e = 0; e < 2; ++e) {quad_pts[ix++].assign(p.x, p.y, part.d[2][d^e]);}
		}
		vector3d const normal(get_poly_norm(quad_pts));
		float const rdist(dot_product_ptv(normal, pos, quad_pts[0]));
		if (rdist < 0.0 || rdist >= radius) continue; // too far or wrong side
		if (!sphere_poly_intersect(quad_pts, 4, pos, normal, rdist, radius)) continue;
		pos += normal*(radius - rdist);
		if (cnorm) {*cnorm = normal;}
		updated = 1;
	} // for S
	if (updated) return 1;

	if (max(pos.z, p_last.z) > part.z2() && point_in_polygon_2d(pos.x, pos.y, points.data(), num_sides, 0, 1)) { // test top plane (sphere on top of polygon?)
		pos.z = part.z2() + radius; // make sure it doesn't intersect the roof
		if (cnorm) {*cnorm = plus_z;}
		return 1;
	}
	return 0;
}

bool building_t::check_sphere_coll(point &pos, point const &p_last, vector3d const &xlate, float radius,
	bool xy_only, vector<point> &points, vector3d *cnorm_ptr, bool check_interior) const
{
	if (!is_valid()) return 0; // invalid building
	point p_int;
	vector3d cnorm; // unused
	unsigned cdir(0); // unused
	if (radius > 0.0 && !sphere_cube_intersect(pos, radius, (bcube + xlate), p_last, p_int, cnorm, cdir, 1, xy_only)) return 0;
	point pos2(pos), p_last2(p_last), center;
	bool had_coll(0), is_interior(0);

	if (is_rotated()) {
		center = bcube.get_cube_center() + xlate;
		do_xy_rotate(-rot_sin, rot_cos, center, pos2); // inverse rotate - negate the sine term
		do_xy_rotate(-rot_sin, rot_cos, center, p_last2);
	}
	for (auto i = parts.begin(); i != parts.end(); ++i) {
		if (xy_only && i->d[2][0] > bcube.d[2][0]) break; // only need to check first level in this mode
		if (!xy_only && ((pos2.z + radius < i->d[2][0] + xlate.z) || (pos2.z - radius > i->d[2][1] + xlate.z))) continue; // test z overlap
		if (radius == 0.0 && !(xy_only ? i->contains_pt_xy(pos2) : i->contains_pt(pos2))) continue; // no intersection; ignores p_last

		if (use_cylinder_coll()) {
			point const cc(i->get_cube_center() + xlate);
			float const crx(0.5*i->dx()), cry(0.5*i->dy()), r_sum(radius + max(crx, cry));
			if (!dist_xy_less_than(pos2, cc, r_sum)) continue; // no intersection

			if (fabs(crx - cry) < radius) { // close to a circle
				if (p_last2.z > i->d[2][1] + xlate.z && dist_xy_less_than(pos2, cc, max(crx, cry))) {
					pos2.z = i->z2() + radius; // make sure it doesn't intersect the roof
					if (cnorm_ptr) {*cnorm_ptr = plus_z;}
				}
				else { // side coll
					vector2d const d((pos2.x - cc.x), (pos2.y - cc.y));
					float const mult(r_sum/d.mag());
					pos2.x = cc.x + mult*d.x;
					pos2.y = cc.y + mult*d.y;
					if (cnorm_ptr) {*cnorm_ptr = vector3d(d.x, d.y, 0.0).get_norm();} // no z-component
				}
				had_coll = 1;
			}
			else {
				had_coll |= test_coll_with_sides(pos2, p_last2, radius, (*i + xlate), points, cnorm_ptr); // use polygon collision test
			}
		}
		else if (num_sides != 4) { // triangle, hexagon, octagon, etc.
			had_coll |= test_coll_with_sides(pos2, p_last2, radius, (*i + xlate), points, cnorm_ptr);
		}
		else if (sphere_cube_int_update_pos(pos2, radius, (*i + xlate), p_last2, 1, xy_only, cnorm_ptr)) { // cube
			had_coll = 1; // flag as colliding, continue to look for more collisions (inside corners)
			if (check_interior && interior != nullptr) {is_interior = 1;}
		}
	} // for i
	if (!xy_only) { // don't need to check details and roof in xy_only mode because they're contained in the XY footprint of the parts
		for (auto i = details.begin(); i != details.end(); ++i) {
			if (sphere_cube_int_update_pos(pos2, radius, (*i + xlate), p_last2, 1, xy_only, cnorm_ptr)) {had_coll = 1;} // cube, flag as colliding
		}
		for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) { // Note: doesn't really work with a pointed roof
			point const pos_xlate(pos2 - xlate);
			vector3d const normal(i->get_norm());
			float const rdist(dot_product_ptv(normal, pos_xlate, i->pts[0]));

			if (fabs(rdist) < radius && sphere_poly_intersect(i->pts, i->npts, pos_xlate, normal, rdist, radius)) {
				pos2 += normal*(radius - rdist); // update current pos
				had_coll = 1; // flag as colliding
				if (cnorm_ptr) {*cnorm_ptr = ((normal.z < 0.0) ? -1.0 : 1.0)*normal;} // make sure normal points up
				break; // only use first colliding tquad
			}
		}
	}
	if (is_interior) {had_coll = check_sphere_coll_interior(pos, p_last, xlate, radius, xy_only, cnorm_ptr);} // sphere collides with cube and check_interior=1
	if (!had_coll) return 0; // Note: no collisions with windows or doors, since they're colinear with walls

	if (is_rotated()) {
		do_xy_rotate(rot_sin, rot_cos, center, pos2); // rotate back around center
		if (cnorm_ptr) {do_xy_rotate(rot_sin, rot_cos, all_zeros, *cnorm_ptr);} // rotate back (pure rotation)
	}
	pos = pos2;
	return had_coll;
}

// Note: pos and p_last are already in rotated coordinate space
// TODO_INT: default player is actually too large to fit through doors and too tall to fit between the floor and celing, so this doesn't really work
bool building_t::check_sphere_coll_interior(point &pos, point const &p_last, vector3d const &xlate, float radius, bool xy_only, vector3d *cnorm) const {
	assert(interior);
	bool had_coll(0);

	for (unsigned d = 0; d < 2; ++d) { // check XY collision with walls
		for (auto i = interior->walls[d].begin(); i != interior->walls[d].end(); ++i) {
			had_coll |= sphere_cube_int_update_pos(pos, radius, (*i + xlate), p_last, 1, 1, cnorm); // skip_z=1
		}
		had_coll = 0; // coll logic not yet working, so don't register this as a coll
	}
	if (!xy_only && 2.2*radius < get_window_vspace()*(1.0 - FLOOR_THICK_VAL)) { // diameter is smaller than space between floor and ceiling
		// check Z collision with floors; no need to check ceilings
		for (auto i = interior->floors.begin(); i != interior->floors.end(); ++i) {
			//had_coll |= sphere_cube_int_update_pos(pos, radius, (*i + xlate), p_last, 1, 0, cnorm);
			if (!sphere_cube_intersect((pos - xlate), radius, *i)) continue;
			if (pos.z < i->z1() + xlate.z) {pos.z = i->z1() - i->dz() + xlate.z - radius;} // move down below ceiling
			else                           {pos.z = i->z2() + xlate.z + radius;} // move up above floor
			had_coll = 1;
			break; // only change zval once
		}
	}
	if (interior->room_geom) { // collision with room cubes
		vector<room_object_t> const &objs(interior->room_geom->objs);
		for (auto c = objs.begin(); c != objs.end(); ++c) {had_coll |= sphere_cube_int_update_pos(pos, radius, (*c + xlate), p_last, 1, 1, cnorm);} // skip_z=1???
	}
	return had_coll;
}

unsigned building_t::check_line_coll(point const &p1, point const &p2, vector3d const &xlate, float &t, vector<point> &points,
	bool occlusion_only, bool ret_any_pt, bool no_coll_pt) const
{
	if (!check_line_clip(p1-xlate, p2-xlate, bcube.d)) return 0; // no intersection
	point p1r(p1), p2r(p2);
	float tmin(0.0), tmax(1.0);
	unsigned coll(0); // 0=none, 1=side, 2=roof, 3=details

	if (is_rotated()) {
		point const center(bcube.get_cube_center() + xlate);
		do_xy_rotate(-rot_sin, rot_cos, center, p1r); // inverse rotate - negate the sine term
		do_xy_rotate(-rot_sin, rot_cos, center, p2r);
	}
	p1r -= xlate; p2r -= xlate;
	float const pzmin(min(p1r.z, p2r.z)), pzmax(max(p1r.z, p2r.z));
	bool const vert(p1r.x == p2r.x && p1r.y == p2r.y);

	for (auto i = parts.begin(); i != parts.end(); ++i) {
		if (pzmin > i->z2() || pzmax < i->z1()) continue; // no overlap in z
		bool hit(0);

		if (use_cylinder_coll()) { // vertical cylinder
			// Note: we know the line intersects the cylinder's bcube, and there's a good chance it intersects the cylinder, so we don't need any expensive early termination cases here
			point const cc(i->get_cube_center());
			vector3d const csz(i->get_size());

			if (vert) { // vertical line + vertical cylinder optimization + handling of ellipsoids
				if (!point_in_ellipse(p1r, cc, 0.5*csz.x, 0.5*csz.y)) continue; // no intersection (below test should return true as well)
				tmin = (i->z2() - p1r.z)/(p2r.z - p1r.z);
				if (tmin >= 0.0 && tmin < t) {t = tmin; hit = 1;}
			}
			else {
				float const radius(0.5*(occlusion_only ? min(csz.x, csz.y) : max(csz.x, csz.y))); // use conservative radius unless this is an occlusion query
				point const cp1(cc.x, cc.y, i->z1()), cp2(cc.x, cc.y, i->z2());
				if (!line_int_cylinder(p1r, p2r, cp1, cp2, radius, radius, 1, tmin) || tmin > t) continue; // conservative for non-occlusion rays

				if (!occlusion_only && csz.x != csz.y) { // ellipse
					vector3d const delta(p2r - p1r);
					float const rx_inv_sq(1.0/(0.25*csz.x*csz.x)), ry_inv_sq(1.0/(0.25*csz.y*csz.y));
					float t_step(0.1*max(csz.x, csz.y)/delta.mag());

					for (unsigned n = 0; n < 10; ++n) { // use an interative approach
						if (point_in_ellipse_risq((p1r + tmin*delta), cc, rx_inv_sq, ry_inv_sq)) {hit = 1; tmin -= t_step;} else {tmin += t_step;}
						if (hit) {t_step *= 0.5;} // converge on hit point
					}
					if (!hit) continue; // not actually a hit
				} // end ellipse case
				t = tmin; hit = 1;
			}
		}
		else if (num_sides != 4) {
			building_draw_utils::calc_poly_pts(*this, *i, points);
			float const tz((i->z2() - p1r.z)/(p2r.z - p1r.z)); // t value at zval = top of cube

			if (tz >= 0.0 && tz < t) {
				float const xval(p1r.x + tz*(p2r.x - p1r.x)), yval(p1r.y + tz*(p2r.y - p1r.y));
				if (point_in_polygon_2d(xval, yval, points.data(), points.size(), 0, 1)) {t = tz; hit = 1;} // XY plane test for vertical lines and top surface
			}
			if (!vert) { // test building sides
				point quad_pts[4]; // quads

				for (unsigned S = 0; S < num_sides; ++S) { // generate vertex data quads
					for (unsigned d = 0, ix = 0; d < 2; ++d) {
						point const &p(points[(S+d)%num_sides]);
						for (unsigned e = 0; e < 2; ++e) {quad_pts[ix++].assign(p.x, p.y, i->d[2][d^e]);}
					}
					if (line_poly_intersect(p1r, p2r, quad_pts, 4, get_poly_norm(quad_pts), tmin) && tmin < t) {t = tmin; hit = 1;} // Note: untested
				} // for S
			}
		}
		else if (get_line_clip(p1r, p2r, i->d, tmin, tmax) && tmin < t) {t = tmin; hit = 1;} // cube

		if (hit) {
			if (occlusion_only) return 1; // early exit
			if (vert) {coll = 2;} // roof
			else {
				float const zval(p1.z + t*(p2.z - p1.z));
				coll = ((fabs(zval - i->d[2][1]) < 0.0001*i->dz()) ? 2 : 1); // test if clipped zval is close to the roof zval
			}
			if (ret_any_pt) return coll;
		}
	} // for i
	if (occlusion_only) return 0;

	for (auto i = details.begin(); i != details.end(); ++i) {
		if (get_line_clip(p1r, p2r, i->d, tmin, tmax) && tmin < t) {t = tmin; coll = 3;} // details cube
	}
	if (!no_coll_pt || !vert) { // vert line already tested building cylins/cubes, and marked coll roof, no need to test again unless we need correct coll_pt t-val
		for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {
			if (line_poly_intersect(p1r, p2r, i->pts, i->npts, i->get_norm(), tmin) && tmin < t) {t = tmin; coll = 2;} // roof quad
		}
	}
	return coll; // Note: no collisions with windows or doors, since they're colinear with walls; no collision with interior for now
}

// Note: if xy_radius == 0.0, this is a point test; otherwise, it's an approximate vertical cylinder test
bool building_t::check_point_or_cylin_contained(point const &pos, float xy_radius, vector<point> &points) const {

	if (xy_radius == 0.0 && !bcube.contains_pt(pos)) return 0; // no intersection
	point pr(pos);
	if (is_rotated()) {do_xy_rotate(-rot_sin, rot_cos, bcube.get_cube_center(), pr);} // inverse rotate - negate the sine term

	for (auto i = parts.begin(); i != parts.end(); ++i) {
		if (pr.z > i->z2() || pr.z < i->z1()) continue; // no overlap in z

		if (use_cylinder_coll()) { // vertical cylinder
			point const cc(i->get_cube_center());
			vector3d const csz(i->get_size());
			float const dx(cc.x - pr.x), dy(cc.y - pr.y), rx(0.5*csz.x + xy_radius), ry(0.5*csz.y + xy_radius);
			if (dx*dx/(rx*rx) + dy*dy/(ry*ry) > 1.0f) continue; // no intersection (below test should return true as well)
			return 1;
		}
		else if (num_sides != 4) {
			building_draw_utils::calc_poly_pts(*this, *i, points);

			if (xy_radius > 0.0) { // cylinder case: expand polygon by xy_radius; assumes a convex polygon
				point const center(i->get_cube_center());

				for (auto p = points.begin(); p != points.end(); ++p) {
					vector3d dir(*p - center);
					dir.z = 0.0; // only want XY component
					*p += dir*(xy_radius/dir.mag());
				}
			}
			if (point_in_polygon_2d(pr.x, pr.y, &points.front(), points.size(), 0, 1)) return 1; // XY plane test for top surface
		}
		else { // cube
			if (xy_radius > 0.0) {
				cube_t cube(*i);
				cube.expand_by(xy_radius);
				if (cube.contains_pt(pr)) return 1;
			}
			else if (i->contains_pt(pr)) return 1;
		}
	} // for i
	return 0;
}

void building_t::calc_bcube_from_parts() {
	assert(!parts.empty());
	bcube = parts[0];
	for (auto i = parts.begin()+1; i != parts.end(); ++i) {bcube.union_with_cube(*i);} // update bcube
}

void building_t::adjust_part_zvals_for_floor_spacing(cube_t &c) const {

	if (!EXACT_MULT_FLOOR_HEIGHT) return;
	float const floor_spacing(get_window_vspace()), dz(c.dz());
	assert(dz > 0.0 && floor_spacing > 0.0);
	float const num_floors(dz/floor_spacing);
	int const targ_num_floors(max(1, round_fp(num_floors)));
	c.z2() += floor_spacing*(targ_num_floors - num_floors); // ensure c.dz() is an exact multiple of num_floors
}

void split_cubes_recur(cube_t c, vect_cube_t &cubes, unsigned search_start, unsigned search_end) {

	for (unsigned i = search_start; i < search_end; ++i) {
		cube_t const &sc(cubes[i]);
		assert(sc.z2() >= c.z2()); // assumes cubes are ordered descending by ztop
		if (!sc.intersects_no_adj(c)) continue;
		if (sc.contains_cube(c)) return; // contained, done (remove all of c)
		// find a split plane
		for (unsigned d = 0; d < 2; ++d) { // dim
			for (unsigned e = 0; e < 2; ++e) { // dir
				float const split_pos(cubes[i].d[d][e]); // Note: can't use sc reference as it may have been invalidated by a push_back()
				
				if (split_pos > c.d[d][0] && split_pos < c.d[d][1]) { // this plane splits c
					cube_t hi_c(c);
					hi_c.d[d][0] = split_pos; // hi part
					c.   d[d][1] = split_pos; // lo part
					// recursively split the hi part starting at this cube if it's not contained; this split plane will no longer be active
					if (!cubes[i].contains_cube(hi_c)) {split_cubes_recur(hi_c, cubes, i, search_end);}
					if ( cubes[i].contains_cube(c)) return; // done (optimization)
				}
			} // for e
		} // for d
	} // for i
	cubes.push_back(c);
}

void building_t::gen_geometry(int rseed1, int rseed2) {

	if (!is_valid()) return; // invalid building
	if (!parts.empty()) {adjust_part_zvals_for_floor_spacing(parts.front());}
	cube_t const base(parts.empty() ? bcube : parts.back());
	assert(base.is_strictly_normalized());
	parts.clear();
	details.clear();
	roof_tquads.clear();
	doors.clear();
	interior.reset();
	building_mat_t const &mat(get_material());
	rand_gen_t rgen;
	rgen.set_state(123+rseed1, 345*rseed2);
	ao_bcz2 = bcube.z2(); // capture z2 before union with roof and detail geometry (which increases building height)
	if (is_house) {gen_house(base, rgen); return;}

	// determine building shape (cube, cylinder, other)
	if (rgen.rand_probability(mat.round_prob)) {num_sides = MAX_CYLIN_SIDES;} // max number of sides for drawing rounded (cylinder) buildings
	else if (rgen.rand_probability(mat.cube_prob)) {num_sides = 4;} // cube
	else { // N-gon
		num_sides = mat.min_sides;
		if (mat.min_sides != mat.max_sides) {num_sides += (rgen.rand() % (1 + abs((int)mat.max_sides - (int)mat.min_sides)));}
	}
	bool const was_cube(is_cube()); // before num_sides increase due to ASF

	if (num_sides >= 6 && mat.max_fsa > 0.0) { // at least 6 sides
		flat_side_amt = max(0.0f, min(0.45f, rgen.rand_uniform(mat.min_fsa, mat.max_fsa)));
		if (flat_side_amt > 0.0 && rot_sin == 0.0) {start_angle = rgen.rand_uniform(0.0, TWO_PI);} // flat side, not rotated: add random start angle to break up uniformity
	}
	if ((num_sides == 3 || num_sides == 4 || num_sides == 6) && mat.max_asf > 0.0 && rgen.rand_probability(mat.asf_prob)) { // triangles/cubes/hexagons
		alt_step_factor = max(0.0f, min(0.99f, rgen.rand_uniform(mat.min_asf, mat.max_asf)));
		if (alt_step_factor > 0.0 && !(num_sides&1)) {half_offset = 1;} // chamfered cube/hexagon
		if (alt_step_factor > 0.0) {num_sides *= 2;}
	}

	// determine the number of levels and splits
	unsigned num_levels(mat.min_levels);

	if (mat.min_levels < mat.max_levels) { // have a range of levels
		if (was_cube || rgen.rand_bool()) {num_levels += rgen.rand() % (mat.max_levels - mat.min_levels + 1);} // only half of non-cubes are multilevel (unless min_level > 1)
	}
	if (mat.min_level_height > 0.0) {num_levels = max(mat.min_levels, min(num_levels, unsigned(bcube.get_size().z/mat.min_level_height)));}
	num_levels = max(num_levels, 1U); // min_levels can be zero to apply more weight to 1 level buildings
	bool const do_split(num_levels < 4 && is_cube() && rgen.rand_probability(mat.split_prob)); // don't split buildings with 4 or more levels, or non-cubes

	if (num_levels == 1) { // single level
		if (do_split) {split_in_xy(base, rgen);} // generate L, T, or U shape
		else { // single part, entire cube/cylinder
			parts.push_back(base);
			if ((rgen.rand()&3) != 0) {gen_sloped_roof(rgen);} // 75% chance
			gen_details(rgen);
		}
		gen_interior(rgen, 0);
		gen_building_doors_if_needed(rgen);
		return; // for now the bounding cube
	}
	// generate building levels and splits
	float const height(base.dz()), dz(height/num_levels);
	assert(height > 0.0);

	if (!do_split && (rgen.rand()&3) < (was_cube ? 2 : 3)) { // oddly shaped multi-sided overlapping sections (50% chance for cube buildings and 75% chance for others)
		point const llc(base.get_llc()), sz(base.get_size());
		float const abs_min_edge_move(0.5*mat.get_floor_spacing()); // same as door width
		parts.reserve(num_levels); // at least this many

		for (unsigned i = 0; i < num_levels; ++i) { // generate overlapping cube levels, tallest to shortest
			cube_t bc(base); // copy from base to start, keep z1
			bc.z2() = base.z1() + (num_levels - i)*dz; // z2
			if (i > 0) {bc.z2() += dz*rgen.rand_uniform(-0.45, 0.45); bc.z2() = min(bc.z2(), base.z2());}
			if (i > 0) {assert(bc.z2() <= parts.back().z2());}
			assert(bc.is_strictly_normalized());
			adjust_part_zvals_for_floor_spacing(bc);
			bool valid(0);

			// make 100 attempts to generate a cube that isn't contained in any existing cubes; most of the time should pass the first time, so it should never actually fail
			for (unsigned n = 0; n < 100; ++n) {
				for (unsigned d = 0; d < 2; ++d) { // x,y
					float const mv_lo(rgen.rand_uniform(-0.2, 0.45)), mv_hi(rgen.rand_uniform(-0.2, 0.45));
					if (mv_lo > 0.0) {bc.d[d][0] = base.d[d][0] + max(abs_min_edge_move, mv_lo*sz[d]);}
					if (mv_hi > 0.0) {bc.d[d][1] = base.d[d][1] - max(abs_min_edge_move, mv_hi*sz[d]);}
				}
				assert(bc.is_strictly_normalized());
				bool contained(0);
				for (auto p = parts.begin(); p != parts.end(); ++p) {contained |= p->contains_cube(bc);}
				if (!contained) {valid = 1; break;} // success
			} // for n
			if (!valid) break; // remove this part and end the building here
			if (i == 0 || !is_simple_cube()) {parts.push_back(bc); continue;} // no splitting
			split_cubes_recur(bc, parts, 0, parts.size()); // split this cube against all previously added cubes and remove overlapping areas
		} // for i
		parts.shrink_to_fit(); // optional
		std::reverse(parts.begin(), parts.end()); // highest part should be last so that it gets the roof details
		calc_bcube_from_parts(); // update bcube
		gen_details(rgen);
		gen_interior(rgen, 1);
		gen_building_doors_if_needed(rgen);
		return;
	}
	parts.resize(num_levels);

	for (unsigned i = 0; i < num_levels; ++i) {
		cube_t &bc(parts[i]);
		if (i == 0) {bc = base;} // use full building footprint
		else {
			cube_t const &prev(parts[i-1]);
			float const shift_mult(was_cube ? 1.0 : 0.5); // half the shift for non-cube buildings

			for (unsigned d = 0; d < 2; ++d) {
				float const len(prev.get_sz_dim(d)), min_edge_len((0.2f/shift_mult)*(bcube.get_sz_dim(d)));
				bool const inv(rgen.rand_bool());

				for (unsigned e = 0; e < 2; ++e) {
					float delta(0.0);
					if (rgen.rand()&3) {delta = shift_mult*rgen.rand_uniform(0.1, 0.4);} // 25% chance of no shift, 75% chance of 20-40% shift
					bc.d[d][e] = prev.d[d][e] + (e ? -delta : delta)*len;
				}
				for (unsigned E = 0; E < 2; ++E) {
					bool const e((E != 0) ^ inv); // no dir favoritism for 20% check
					if (bc.get_sz_dim(d) < min_edge_len) {bc.d[d][e] = prev.d[d][e];} // if smaller than 20% base width, revert the change
				}
			}
			bc.z1() = prev.z2(); // z1
		}
		bc.z2() = bc.z1() + dz; // z2
		bc.normalize(); // handle XY inversion due to shift
	} // for i
	for (unsigned i = 1; i < num_levels; ++i) {
		float const ddz(rgen.rand_uniform(-0.35*dz, 0.35*dz)); // random shift in z height
		parts[i-1].z2() += ddz;
		adjust_part_zvals_for_floor_spacing(parts[i-1]);
		parts[i].z1() = parts[i-1].z2(); // make top and bottom parts align
	}
	adjust_part_zvals_for_floor_spacing(parts[num_levels-1]); // last one
	max_eq(bcube.z2(), parts[num_levels-1].z2()); // adjust bcube if needed

	if (do_split) { // generate L, T, or U shape
		cube_t const split_cube(parts.back());
		parts.pop_back();
		split_in_xy(split_cube, rgen);
	}
	else {
		if ((rgen.rand()&3) != 0) {gen_sloped_roof(rgen);} // 67% chance
		if (num_levels <= 3) {gen_details(rgen);}
	}
	gen_interior(rgen, 0);
	gen_building_doors_if_needed(rgen);
}

void building_t::split_in_xy(cube_t const &seed_cube, rand_gen_t &rgen) {

	// generate L, T, U, H, + shape
	point const llc(seed_cube.get_llc()), sz(seed_cube.get_size());
	int const shape(rand()%9); // 0-8
	bool const is_hp(shape >= 7);
	bool const dim(rgen.rand_bool()); // {x,y}
	bool const dir(is_hp ? 1 : rgen.rand_bool()); // {neg,pos} - H/+ shapes are always pos
	float const div(is_hp ? rgen.rand_uniform(0.2, 0.4) : rgen.rand_uniform(0.3, 0.7)), s1(rgen.rand_uniform(0.2, 0.4)), s2(rgen.rand_uniform(0.6, 0.8)); // split pos in 0-1 range
	float const dpos(llc[dim] + div*sz[dim]), spos1(llc[!dim] + s1*sz[!dim]), spos2(llc[!dim] + s2*sz[!dim]); // split pos in cube space
	unsigned const start(parts.size()), num((shape >= 6) ? 3 : 2);
	parts.resize(start+num, seed_cube);
	parts[start+0].d[dim][ dir] = dpos; // full width part (except +)
	parts[start+1].d[dim][!dir] = dpos; // partial width part (except +)

	switch (shape) {
	case 0: case 1: case 2: case 3: // L
		parts[start+1].d[!dim][shape>>1] = ((shape&1) ? spos2 : spos1);
		break;
	case 4: case 5: // T
		parts[start+1].d[!dim][0] = spos1;
		parts[start+1].d[!dim][1] = spos2;
		break;
	case 6: // U
		parts[start+2].d[ dim][!dir] = dpos; // partial width part
		parts[start+1].d[!dim][1   ] = spos1;
		parts[start+2].d[!dim][0   ] = spos2;
		break;
	case 7: { // H
		float const dpos2(llc[dim] + (1.0 - div)*sz[dim]); // other end
		parts[start+1].d[ dim][ dir] = dpos2;
		parts[start+1].d[!dim][ 0  ] = spos1; // middle part
		parts[start+1].d[!dim][ 1  ] = spos2;
		parts[start+2].d[ dim][!dir] = dpos2; // full width part
		break;
	}
	case 8: { // +
		float const dpos2(llc[dim] + (1.0 - div)*sz[dim]); // other end
		parts[start+0].d[!dim][ 0  ] = spos1;
		parts[start+0].d[!dim][ 1  ] = spos2;
		parts[start+2].d[!dim][ 0  ] = spos1;
		parts[start+2].d[!dim][ 1  ] = spos2;
		parts[start+1].d[ dim][ dir] = dpos2; // middle part
		parts[start+2].d[ dim][!dir] = dpos2; // partial width part
		break;
	}
	default: assert(0);
	}
}

bool get_largest_xy_dim(cube_t const &c) {return (c.dy() > c.dx());}

cube_t building_t::place_door(cube_t const &base, bool dim, bool dir, float door_height, float door_center, float door_pos, float door_center_shift, float width_scale, rand_gen_t &rgen) {

	if (door_center == 0.0) { // door not yet calculated; add door to first part of house
		bool const centered(door_center_shift == 0.0 || hallway_dim == (unsigned char)dim); // center doors connected to primary hallways
		float const offset(centered ? 0.5 : rgen.rand_uniform(0.5-door_center_shift, 0.5+door_center_shift));
		door_center = offset*base.d[!dim][0] + (1.0 - offset)*base.d[!dim][1];
		door_pos    = base.d[dim][dir];
	}
	float const door_half_width(0.5*width_scale*door_height);
	float const door_shift((is_house ? 0.005 : 0.001)*base.dz());

	if (interior && hallway_dim == 2) { // not on a hallway
		vect_cube_t const &walls(interior->walls[!dim]); // perpendicular to door
		float const door_lo(door_center - 1.2*door_half_width), door_hi(door_center + 1.2*door_half_width); // pos along wall with a small expand
		float const dpos_lo(door_pos    -     door_half_width), dpos_hi(door_pos    +     door_half_width); // expand width of the door

		for (auto w = walls.begin(); w != walls.end(); ++w) {
			if (w->d[ dim][0] > dpos_hi || w->d[ dim][1] < dpos_lo) continue; // not ending at same wall as door
			if (w->d[!dim][0] > door_hi || w->d[!dim][1] < door_lo) continue; // not intersecting door
			// Note: since we know that all rooms are wider than the door width, we know that we have space for a door on either side of the wall
			float const lo_dist(w->d[!dim][0] - door_lo), hi_dist(door_hi - w->d[!dim][1]);
			if (lo_dist < hi_dist) {door_center += lo_dist;} else {door_center -= hi_dist;} // move the door so that it doesn't open into the end of the wall
			break;
		}
	}
	cube_t door;
	door.z1() = base.z1(); // same bottom as house
	door.z2() = door.z1() + door_height;
	door.d[ dim][!dir] = door_pos + door_shift*(dir ? 1.0 : -1.0); // move slightly away from the house to prevent z-fighting
	door.d[ dim][ dir] = door.d[dim][!dir]; // make zero size in this dim
	door.d[!dim][0] = door_center - door_half_width; // left
	door.d[!dim][1] = door_center + door_half_width; // right
	return door;
}

void building_t::gen_house(cube_t const &base, rand_gen_t &rgen) {

	assert(parts.empty());
	int const type(rgen.rand()%3); // 0=single cube, 1=L-shape, 2=two-part
	bool const two_parts(type != 0);
	unsigned force_dim[2] = {2}; // force roof dim to this value, per part; 2 = unforced/auto
	bool skip_last_roof(0);
	num_sides = 4;
	parts.reserve(two_parts ? 5 : 2); // two house sections + porch roof + porch support + chimney (upper bound)
	parts.push_back(base);
	// add a door
	bool const gen_door(global_building_params.windows_enabled());
	float door_height(get_door_height()), door_center(0.0), door_pos(0.0);
	bool door_dim(rgen.rand_bool()), door_dir(0);
	unsigned door_part(0);

	if (two_parts) { // multi-part house; parts[1] is the lower height part
		parts.push_back(base); // add second part
		bool const dir(rgen.rand_bool()); // in dim
		float const split(rgen.rand_uniform(0.4, 0.6)*(dir  ? -1.0 : 1.0));
		float delta_height(0.0), shrink[2] = {0.0};
		bool dim(0), dir2(0);

		if (type == 1) { // L-shape
			dir2         = rgen.rand_bool(); // in !dim
			dim          = rgen.rand_bool();
			shrink[dir2] = rgen.rand_uniform(0.4, 0.6)*(dir2 ? -1.0 : 1.0);
			delta_height = max(0.0f, rand_uniform(-0.1, 0.5));
		}
		else if (type == 2) { // two-part
			dim          = get_largest_xy_dim(base); // choose longest dim
			delta_height = rand_uniform(0.1, 0.5);

			for (unsigned d = 0; d < 2; ++d) {
				if (rgen.rand_bool()) {shrink[d] = rgen.rand_uniform(0.2, 0.35)*(d ? -1.0 : 1.0);}
			}
		}
		vector3d const sz(base.get_size());
		parts[0].d[ dim][ dir] += split*sz[dim]; // split in dim
		parts[1].d[ dim][!dir]  = parts[0].d[dim][dir];
		cube_t const pre_shrunk_p1(parts[1]); // save for use in details below
		for (unsigned d = 0; d < 2; ++d) {parts[1].d[!dim][d] += shrink[d]*sz[!dim];} // shrink this part in the other dim
		parts[1].z2() -= delta_height*parts[1].dz(); // lower height
		if (ADD_BUILDING_INTERIORS) {adjust_part_zvals_for_floor_spacing(parts[1]);}
		if (type == 1 && rgen.rand_bool()) {force_dim[0] = !dim; force_dim[1] = dim;} // L-shape, half the time
		else if (type == 2) {force_dim[0] = force_dim[1] = dim;} // two-part - force both parts to have roof along split dim
		int const detail_type((type == 1) ? (rgen.rand()%3) : 0); // 0=none, 1=porch, 2=detatched garage/shed
		door_dir = ((door_dim == dim) ? dir : dir2); // if we have a porch/shed/garage, put the door on that side
		if (door_dim == dim && detail_type == 0) {door_dir ^= 1;} // put it on the opposite side so that the second part isn't in the way

		if (detail_type != 0) { // add details to L-shaped house
			cube_t c(pre_shrunk_p1);
			c.d[!dim][!dir2] = parts[1].d[!dim][dir2]; // other half of the shrunk part1
			float const dist1((c.d[!dim][!dir2] - base.d[!dim][dir2])*rgen.rand_uniform(0.4, 0.6));
			float const dist2((c.d[ dim][!dir ] - base.d[ dim][dir ])*rgen.rand_uniform(0.4, 0.6));
			float const height(rgen.rand_uniform(0.55, 0.7)*parts[1].dz());

			if (gen_door) { // add door in interior of L, centered under porch roof (if it exists, otherwise where it would be)
				door_center = c.get_center_dim(!door_dim) + 0.5f*((door_dim == dim) ? dist1 : dist2);
				door_pos    = c.d[door_dim][!door_dir];
				door_part   = ((door_dim == dim) ? 0 : 1); // which part the door is connected to
				min_eq(door_height, 0.95f*height);
			}
			if (detail_type == 1) { // porch
				float const width(0.05f*(fabs(dist1) + fabs(dist2))); // width of support pillar
				c.d[!dim][dir2 ] += dist1; // move away from bcube edge
				c.d[ dim][ dir ] += dist2; // move away from bcube edge
				c.d[!dim][!dir2] -= 0.001*dist1; // adjust slightly so it's not exactly adjacent to the house and won't be considered internal face removal logic
				c.d[ dim][ !dir] -= 0.001*dist2;
				c.z1() += height; // move up
				c.z2()  = c.z1() + 0.05*parts[1].dz();
				parts.push_back(c); // porch roof
				c.z2() = c.z1();
				c.z1() = pre_shrunk_p1.z1(); // support pillar
				c.d[!dim][!dir2] = c.d[!dim][dir2] + (dir2 ? -1.0 : 1.0)*width;
				c.d[ dim][!dir ] = c.d[ dim][ dir] + (dir  ? -1.0 : 1.0)*width;
				skip_last_roof = 1;
			}
			else if (detail_type == 2) { // detatched garage/shed
				c.d[!dim][dir2 ]  = base.d[!dim][dir2]; // shove it into the opposite corner of the bcube
				c.d[ dim][dir  ]  = base.d[ dim][dir ]; // shove it into the opposite corner of the bcube
				c.d[!dim][!dir2] -= dist1; // move away from bcube edge
				c.d[ dim][!dir ] -= dist2; // move away from bcube edge
				c.z2() = c.z1() + min(min(c.dx(), c.dy()), height); // no taller than x or y size; Note: z1 same as part1
			}
			parts.push_back(c); // support column or shed/garage
		} // end house details
		calc_bcube_from_parts(); // maybe calculate a tighter bounding cube
	} // end type != 0  (multi-part house)
	else if (gen_door) { // single cube house
		door_dir  = rgen.rand_bool(); // select a random dir
		door_part = 0; // only one part
	}
	gen_interior(rgen, 0); // before adding door
	if (gen_door) {add_door(place_door(parts[door_part], door_dim, door_dir, door_height, door_center, door_pos, 0.25, 0.5, rgen), door_part, door_dim, door_dir, 0);}
	float const peak_height(rgen.rand_uniform(0.15, 0.5)); // same for all parts
	float roof_dz[3] = {0.0f};

	for (auto i = parts.begin(); (i + skip_last_roof) != parts.end(); ++i) {
		unsigned const ix(i - parts.begin()), fdim(force_dim[ix]);
		cube_t const &other(two_parts ? parts[1-ix] : *i); // == self for single part houses
		bool const dim((fdim < 2) ? fdim : get_largest_xy_dim(*i)); // use longest side if not forced
		bool const other_dim(two_parts ? ((force_dim[1-ix] < 2) ? force_dim[1-ix] : get_largest_xy_dim(other)) : 0);
		float extend_to(0.0), max_dz(i->dz());

		if (type == 1 && ix == 1 && dim != other_dim && parts[0].z2() == parts[1].z2()) { // same z2, opposite dim T-junction
			max_dz    = peak_height*parts[0].get_sz_dim(!other_dim); // clamp roof zval to other roof's peak
			extend_to = parts[0].get_center_dim(!other_dim); // extend lower part roof to center of upper part roof
		}
		bool can_be_hipped(ix < (1U + two_parts) && extend_to == 0.0 && i->get_sz_dim(dim) > i->get_sz_dim(!dim)); // must be longer dim
		
		if (can_be_hipped && two_parts) {
			float const part_roof_z (i->z2()    + min(peak_height*i->get_sz_dim(!dim), i->dz()));
			float const other_roof_z(other.z2() + min(peak_height*other.get_sz_dim(!other_dim), other.dz()));
			can_be_hipped = (part_roof_z >= other_roof_z); // no hipped for lower part
		}
		bool const hipped(can_be_hipped && rgen.rand_bool()); // hipped roof 50% of the time
		if (hipped) {roof_dz[ix] = gen_hipped_roof(*i, peak_height, extend_to);}
		else {
			unsigned skip_side_tri(2); // default = skip neither
			
			if (two_parts && dim == other_dim && i->d[!dim][0] >= other.d[!dim][0] && i->d[!dim][1] <= other.d[!dim][1] && i->z2() <= other.z2()) { // side of i contained in other
				for (unsigned d = 0; d < 2; ++d) {
					if (i->d[dim][d] == other.d[dim][!d]) {skip_side_tri = d;} // remove smaller of two opposing/overlapping triangles to prevent z-fighting
				}
			}
			roof_dz[ix] = gen_peaked_roof(*i, peak_height, dim, extend_to, max_dz, skip_side_tri);
		}
	}
	if ((rgen.rand()%3) != 0) { // add a chimney 67% of the time
		unsigned part_ix(0);

		if (two_parts) { // start by selecting a part (if two parts)
			float const v0(parts[0].get_volume()), v1(parts[1].get_volume());
			if      (v0 > 2.0*v1) {part_ix = 0;} // choose larger part 0
			else if (v1 > 2.0*v0) {part_ix = 1;} // choose larger part 1
			else {part_ix = rgen.rand_bool();} // close in area - choose a random part
		}
		unsigned const fdim(force_dim[part_ix]);
		cube_t const &part(parts[part_ix]);
		bool const dim((fdim < 2) ? fdim : get_largest_xy_dim(part)); // use longest side if not forced
		bool dir(rgen.rand_bool());
		if (two_parts && part.d[dim][dir] != bcube.d[dim][dir]) {dir ^= 1;} // force dir to be on the edge of the house bcube (not at a point interior to the house)
		cube_t c(part);
		float const sz1(c.get_sz_dim(!dim)), sz2(c.get_sz_dim(dim));
		float shift(0.0);

		if ((rgen.rand()%3) != 0) { // make the chimney non-centered 67% of the time
			shift = sz1*rgen.rand_uniform(0.1, 0.25); // select a shift in +/- (0.1, 0.25) - no small offset from center
			if (rgen.rand_bool()) {shift = -shift;}
		}
		float const center(c.get_center_dim(!dim) + shift);
		c.d[dim][!dir]  = c.d[dim][ dir] + (dir ? -0.03f : 0.03f)*(sz1 + sz2); // chimney depth
		c.d[dim][ dir] += (dir ? -0.01 : 0.01)*sz2; // slight shift from edge of house to avoid z-fighting
		c.d[!dim][0] = center - 0.05*sz1;
		c.d[!dim][1] = center + 0.05*sz1;
		c.z1()  = c.z2();
		c.z2() += rgen.rand_uniform(1.25, 1.5)*roof_dz[part_ix] - 0.4f*abs(shift);
		parts.push_back(c);
		// add top quad to cap chimney (will also update bcube to contain chimney)
		tquad_t tquad(4); // quad
		tquad.pts[0].assign(c.x1(), c.y1(), c.z2());
		tquad.pts[1].assign(c.x2(), c.y1(), c.z2());
		tquad.pts[2].assign(c.x2(), c.y2(), c.z2());
		tquad.pts[3].assign(c.x1(), c.y2(), c.z2());
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_CCAP); // tag as chimney cap
		has_chimney = 1;
	}
	add_roof_to_bcube();
	gen_grayscale_detail_color(rgen, 0.4, 0.8); // for roof
}

tquad_with_ix_t set_door_from_cube(cube_t const &c, bool dim, bool dir, unsigned type, float pos_adj, bool opened) {

	tquad_with_ix_t door(4, type); // quad
	float const pos(c.d[dim][0] + (opened ? 0.0 : pos_adj*(dir ? 1.0 : -1.0))); // move away from wall slightly (not needed if opened)
	door.pts[0].z = door.pts[1].z = c.z1(); // bottom
	door.pts[2].z = door.pts[3].z = c.z2(); // top
	door.pts[0][!dim] = door.pts[3][!dim] = c.d[!dim][ dir]; //  dir side
	door.pts[1][!dim] = door.pts[2][!dim] = c.d[!dim][!dir]; // !dir side
	door.pts[0][ dim] = door.pts[1][ dim] = door.pts[2][dim] = door.pts[3][dim] = pos;
	if (dim == 0) {swap(door.pts[0], door.pts[1]); swap(door.pts[2], door.pts[3]);} // swap two corner points to flip windowing dir and invert normal for doors oriented in X

	if (opened) { // rotate 90 degrees about pts[0]/pts[3] - change pts[1]/pts[2]; this is just a placeholder for now
		float const width(c.get_sz_dim(!dim));
		door.pts[1][!dim] = door.pts[2][!dim] = door.pts[0][!dim] + 0.01*width*((dir^dim) ? 1.0 : -1.0); // move slightly away from the wall to prevent z-fighting
		door.pts[1][ dim] = door.pts[2][ dim] = door.pts[0][ dim] + width*(dir ? 1.0 : -1.0);
	}
	return door;
}

void building_t::add_door(cube_t const &c, unsigned part_ix, bool dim, bool dir, bool for_building) {

	vector3d const sz(c.get_size());
	assert(sz[dim] == 0.0 && sz[!dim] > 0.0 && sz.z > 0.0);
	unsigned const type(for_building ? (unsigned)tquad_with_ix_t::TYPE_BDOOR : (unsigned)tquad_with_ix_t::TYPE_HDOOR);
	doors.push_back(set_door_from_cube(c, dim, dir, type, 0.01*sz[!dim], 0)); // opened=0
	if (part_ix < 4) {door_sides[part_ix] |= 1 << (2*dim + dir);}
}

unsigned extend_roof(cube_t &top, float extend_to, bool dim) {
	if (extend_to == 0.0) return 2; // extend in neither dim
	if (extend_to < top.d[dim][0]) {top.d[dim][0] = extend_to; return 0;} // lo side extend
	if (extend_to > top.d[dim][1]) {top.d[dim][1] = extend_to; return 1;} // hi side extend
	return 0; // extend in neither dim
}

// roof made from two sloped quads and two triangles
float building_t::gen_peaked_roof(cube_t const &top_, float peak_height, bool dim, float extend_to, float max_dz, unsigned skip_side_tri) {

	cube_t top(top_); // deep copy
	unsigned const extend_dir(extend_roof(top, extend_to, dim));
	float const width(top.get_sz_dim(!dim)), roof_dz(min(max_dz, min(peak_height*width, top.dz())));
	float const z1(top.z2()), z2(z1 + roof_dz), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	point pts[6] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), point(x1, y1, z2), point(x2, y2, z2)};
	if (dim == 0) {pts[4].y = pts[5].y = 0.5f*(y1 + y2);} // yc
	else          {pts[4].x = pts[5].x = 0.5f*(x1 + x2);} // xc
	unsigned const qixs[2][2][4] = {{{0,3,5,4}, {4,5,2,1}}, {{0,4,5,1}, {4,3,2,5}}}; // 2 quads
	roof_tquads.reserve(roof_tquads.size() + (3 + (extend_dir == 2))); // 2 roof quads + 1-2 side triangles
	unsigned const tixs[2][2][3] = {{{1,0,4}, {3,2,5}}, {{0,3,4}, {2,1,5}}}; // 2 triangles

	for (unsigned n = 0; n < 2; ++n) { // triangle section/wall from z1 up to roof
		if (n == extend_dir || n == skip_side_tri) continue; // skip this side
		bool skip(0);

		// exclude tquads contained in/adjacent to other parts, considering only the cube parts;
		// yes, a triangle side can be occluded by a cube + another opposing triangle side from a higher wall of the house, but it's uncommon, complex, and currently ignored
		for (auto p = parts.begin(); p != parts.end(); ++p) {
			if (p->d[dim][!n] != top.d[dim][n]) continue; // not the opposing face
			if (p->z1() <= z1 && p->z2() >= z2 && p->d[!dim][0] <= top.d[!dim][0] && p->d[!dim][1] >= top.d[!dim][1]) {skip = 1; break;}
		}
		if (skip) continue;
		tquad_t tquad(3); // triangle
		UNROLL_3X(tquad.pts[i_] = pts[tixs[dim][n][i_]];);
		if (z1 < bcube.z2() && tquad.pts[2][dim] != bcube.d[dim][n]) {tquad.pts[2][dim] += (n ? -1.0 : 1.0)*0.001*width;} // shift peak in slightly to prevent z-fighting with int walls
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_WALL); // tag as wall
	} // for n
	bool const extend_roof = 0; // disabled for now, but somewhat works

	if (extend_roof) { // extend the roof outside the wall a small amount
		// may require updating bcube for drawing; causes problems with L/T shaped houses with roof intersecting other walls and interiors; requires two sided drawing
		float const extend(0.05*width), extend_dz(2.0*extend*roof_dz/width);
		for (unsigned n = 0; n < 4; ++n) {pts[n].z -= extend_dz;} // so that slope is preserved
		if (dim == 0) {pts[0].y -= extend; pts[1].y += extend; pts[2].y += extend; pts[3].y -= extend;}
		else          {pts[0].x -= extend; pts[1].x -= extend; pts[2].x += extend; pts[3].x += extend;}
	}
	for (unsigned n = 0; n < 2; ++n) { // roof
		tquad_t tquad(4); // quad
		UNROLL_4X(tquad.pts[i_] = pts[qixs[dim][n][i_]];);
		roof_tquads.emplace_back(tquad, (unsigned)tquad_with_ix_t::TYPE_ROOF); // tag as roof
	}
	return roof_dz;
}

// roof made from two sloped quads + two sloped triangles
float building_t::gen_hipped_roof(cube_t const &top_, float peak_height, float extend_to) {

	bool const dim(get_largest_xy_dim(top_)); // always the largest dim
	cube_t top(top_); // deep copy
	unsigned const extend_dir(extend_roof(top, extend_to, dim));
	float const width(top.get_sz_dim(!dim)), length(top.get_sz_dim(dim)), offset(0.5f*(length - width)), roof_dz(min(peak_height*width, top.dz()));
	float const z1(top.z2()), z2(z1 + roof_dz), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	point const center(0.5f*(x1 + x2), 0.5f*(y1 + y2), z2);
	point pts[6] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), center, center};
	if (dim) {UNROLL_3X(swap(pts[0], pts[i_+1]);)} // rotate 1 vertex CCW
	pts[4][dim] -= offset; pts[5][dim] += offset; // move points away from center to create ridgeline
	unsigned const qixs[2][4] = {{0,3,5,4}, {2,1,4,5}};
	unsigned const tixs[2][3] = {{1,0,4}, {3,2,5}};
	unsigned const start_ix(roof_tquads.size());
	roof_tquads.resize(start_ix + 4); // defaults to TYPE_ROOF

	for (unsigned n = 0; n < 2; ++n) {
		unsigned const ix(start_ix + n);
		roof_tquads[ix+0].npts = 4; // quads
		UNROLL_4X(roof_tquads[ix+0].pts[i_] = pts[qixs[n][i_]];)
		roof_tquads[ix+2].npts = 3; // triangles
		UNROLL_3X(roof_tquads[ix+2].pts[i_] = pts[tixs[n][i_]];)
	}
	return roof_dz;
}

void building_t::gen_building_doors_if_needed(rand_gen_t &rgen) {

	if (!is_cube()) return; // for now, only cube buildings can have doors; doors can be added to N-gon (non cylinder) buildings later
	assert(!parts.empty());
	float const door_height(1.1*get_door_height()), wscale(0.7); // a bit taller and a lot wider than house doors

	if (hallway_dim < 2) { // building has primary hallway, place doors at both ends of first part
		for (unsigned d = 0; d < 2; ++d) {
			add_door(place_door(parts.front(), bool(hallway_dim), d, door_height, 0.0, 0.0, 0.0, wscale, rgen), 0, bool(hallway_dim), d, 1);
		}
		return;
	}
	bool const pref_dim(rgen.rand_bool()), pref_dir(rgen.rand_bool());
	bool const has_windows(get_material().add_windows);
	bool used[4] = {0,0,0,0}; // per-side, not per-base cube
	unsigned const num_doors(1 + (rgen.rand() % (has_windows ? 3 : 4))); // 1-4; buildings with windows have at most 3 doors since they're smaller

	for (unsigned num = 0; num < num_doors; ++num) {
		bool placed(0);

		for (auto b = parts.begin(); b != parts.end() && !placed; ++b) { // try all different ground floor parts
			unsigned const part_ix(b - parts.begin());
			if (has_windows && part_ix >= 4) break; // only first 4 parts can have doors - must match first floor window removal logic
			if (b->z1() > bcube.z1()) break; // moved off the ground floor - done

			for (unsigned n = 0; n < 4; ++n) {
				bool const dim(pref_dim ^ bool(n>>1)), dir(pref_dir ^ bool(n&1));
				if (b->d[dim][dir] != bcube.d[dim][dir]) continue; // find a side on the exterior to ensure door isn't obstructed by a building cube
				if (used[2*dim + dir]) continue; // door already placed on this side
				used[2*dim + dir] = 1; // mark used
				add_door(place_door(*b, dim, dir, door_height, 0.0, 0.0, 0.1, wscale, rgen), part_ix, dim, dir, 1);
				placed = 1;
				break;
			} // for n
		} // for b
	} // for num
}

void building_t::gen_details(rand_gen_t &rgen) { // for the roof

	unsigned const num_blocks(roof_tquads.empty() ? (rgen.rand() % 9) : 0); // 0-8; 0 if there are roof quads (houses, etc.)
	has_antenna = (rgen.rand() & 1);
	details.resize(num_blocks + has_antenna);
	assert(!parts.empty());
	if (details.empty()) return; // nothing to do
	cube_t const &top(parts.back()); // top/last part

	if (num_blocks > 0) {
		float const xy_sz(top.get_size().xy_mag());
		float const height_scale(0.0035f*(top.dz() + bcube.dz())); // based on avg height of current section and entire building
		cube_t rbc(top);
		vector<point> points; // reused across calls

		for (unsigned i = 0; i < num_blocks; ++i) {
			cube_t &c(details[i]);
			float const height(height_scale*rgen.rand_uniform(1.0, 4.0));

			while (1) {
				c.set_from_point(point(rgen.rand_uniform(rbc.x1(), rbc.x2()), rgen.rand_uniform(rbc.y1(), rbc.y2()), 0.0));
				c.expand_by(vector3d(xy_sz*rgen.rand_uniform(0.01, 0.08), xy_sz*rgen.rand_uniform(0.01, 0.06), 0.0));
				if (!rbc.contains_cube_xy(c)) continue; // not contained
				if (is_simple_cube()) break; // success/done
				bool contained(1);

				for (unsigned j = 0; j < 4; ++j) { // check cylinder/ellipse
					point const pt(c.d[0][j&1], c.d[1][j>>1], 0.0); // XY only
					if (!check_part_contains_pt_xy(rbc, pt, points)) {contained = 0; break;}
				}
				if (contained) break; // success/done
			} // end while
			c.z1() = top.z2(); // z1
			c.z2() = top.z2() + height; // z2
		} // for i
	}
	if (has_antenna) { // add antenna
		float const radius(0.003f*rgen.rand_uniform(1.0, 2.0)*(top.dx() + top.dy()));
		float const height(rgen.rand_uniform(0.25, 0.5)*top.dz());
		cube_t &antenna(details.back());
		antenna.set_from_point(top.get_cube_center());
		antenna.expand_by(vector3d(radius, radius, 0.0));
		antenna.z1() = top.z2(); // z1
		antenna.z2() = bcube.z2() + height; // z2 (use bcube to include sloped roof)
	}
	for (auto i = details.begin(); i != details.end(); ++i) {max_eq(bcube.z2(), i->z2());} // extend bcube z2 to contain details
	if (roof_tquads.empty()) {gen_grayscale_detail_color(rgen, 0.2, 0.6);} // for antenna and roof
}

void building_t::gen_sloped_roof(rand_gen_t &rgen) { // Note: currently not supported for rotated buildings

	assert(!parts.empty());
	if (!is_simple_cube()) return; // only simple cubes are handled
	cube_t const &top(parts.back()); // top/last part
	float const peak_height(rgen.rand_uniform(0.2, 0.5));
	float const wmin(min(top.dx(), top.dy())), z1(top.z2()), z2(z1 + peak_height*wmin), x1(top.x1()), y1(top.y1()), x2(top.x2()), y2(top.y2());
	point const pts[5] = {point(x1, y1, z1), point(x1, y2, z1), point(x2, y2, z1), point(x2, y1, z1), point(0.5f*(x1 + x2), 0.5f*(y1 + y2), z2)};
	float const d1(rgen.rand_uniform(0.0, 0.8));

	if (d1 < 0.2) { // pointed roof with 4 sloped triangles
		unsigned const ixs[4][3] = {{1,0,4}, {3,2,4}, {0,3,4}, {2,1,4}};
		roof_tquads.resize(4); // defaults to TYPE_ROOF

		for (unsigned n = 0; n < 4; ++n) {
			roof_tquads[n].npts = 3; // triangles
			UNROLL_3X(roof_tquads[n].pts[i_] = pts[ixs[n][i_]];)
		}
	}
	else { // flat roof with center quad and 4 surrounding sloped quads
		point const center((1.0 - d1)*pts[4]);
		point pts2[8];
		for (unsigned n = 0; n < 4; ++n) {pts2[n] = pts[n]; pts2[n+4] = d1*pts[n] + center;}
		unsigned const ixs[5][4] = {{4,7,6,5}, {0,4,5,1}, {3,2,6,7}, {0,3,7,4}, {2,1,5,6}}; // add the flat quad first, which works better for sphere intersections
		roof_tquads.resize(5); // defaults to TYPE_ROOF

		for (unsigned n = 0; n < 5; ++n) {
			roof_tquads[n].npts = 4; // quads
			UNROLL_4X(roof_tquads[n].pts[i_] = pts2[ixs[n][i_]];)
		}
	}
	add_roof_to_bcube();
	//max_eq(bcube.z2(), z2);
	gen_grayscale_detail_color(rgen, 0.4, 0.8); // for antenna and roof
}

void building_t::add_roof_to_bcube() {
	for (auto i = roof_tquads.begin(); i != roof_tquads.end(); ++i) {i->update_bcube(bcube);} // technically should only need to update z2
}
void building_t::gen_grayscale_detail_color(rand_gen_t &rgen, float imin, float imax) {
	float const cscale(rgen.rand_uniform(imin, imax));
	detail_color = colorRGBA(cscale, cscale, cscale, 1.0);
}


// *** Interiors ***

void remove_section_from_cube(cube_t &c, cube_t &c2, float v1, float v2, bool xy) { // c is input+output cube, c2 is other output cube
	//if (!(v1 > c.d[xy][0] && v1 < v2 && v2 < c.d[xy][1])) {cout << TXT(v1) << TXT(v2) << TXT(c.d[xy][0]) << TXT(c.d[xy][1]) << TXT(xy) << endl;}
	assert(v1 > c.d[xy][0] && v1 < v2 && v2 < c.d[xy][1]); // v1/v2 must be interior values for cube
	c2 = c; // clone first cube
	c.d[xy][1] = v1; c2.d[xy][0] = v2; // c=low side, c2=high side
}
void remove_section_from_cube_and_add_door(cube_t &c, cube_t &c2, float v1, float v2, bool xy, vect_cube_t &doors) {
	remove_section_from_cube(c, c2, v1, v2, xy);
	cube_t door(c);
	door.d[!xy][0] = door.d[!xy][1] = c.get_center_dim(!xy); // zero area at wall centerline
	door.d[ xy][0] = v1; door.d[ xy][1] = v2;
	doors.push_back(door);
}
float cube_rand_side_pos(cube_t const &c, int dim, float min_dist_param, float min_dist_abs, rand_gen_t &rgen) {
	assert(dim < 3);
	assert(min_dist_param < 0.5f); // aplies to both ends
	float const lo(c.d[dim][0]), hi(c.d[dim][1]), delta(hi - lo), gap(max(min_dist_abs, min_dist_param*delta));
	//if ((hi-gap) <= (lo+gap)) {cout << TXT(dim) << TXT(lo) << TXT(hi) << TXT(min_dist_abs) << TXT(delta) << TXT(gap) << endl;}
	return rgen.rand_uniform((lo + gap), (hi - gap));
}

// see global_building_params.window_xspace/window_width
int building_t::get_num_windows_on_side(float xy1, float xy2) const {
	assert(xy1 < xy2);
	building_mat_t const &mat(get_material());
	float tscale(2.0f*mat.get_window_tx()), t0(tscale*xy1), t1(tscale*xy2);
	clip_low_high(t0, t1);
	return round_fp(t1 - t0);
}

// Note: wall should start out equal to the room bcube
void create_wall(cube_t &wall, bool dim, float wall_pos, float fc_thick, float wall_half_thick, float wall_edge_spacing) {
	wall.z1() += fc_thick; // start at the floor
	wall.z2() -= fc_thick; // start at the ceiling
	wall.d[ dim][0] = wall_pos - wall_half_thick;
	wall.d[ dim][1] = wall_pos + wall_half_thick;
	// move a bit away from the exterior wall to prevent z-fighting; we might want to add walls around the building exterior and cut window holes
	wall.d[!dim][0] += wall_edge_spacing;
	wall.d[!dim][1] -= wall_edge_spacing;
}

// Note: assumes edge is not clipped and doesn't work when clipped
bool is_val_inside_window(cube_t const &c, bool dim, float val, float window_spacing, float window_border) {
	window_border *= 0.95; // adjust based on window frame so that wall doesn't end right at the edge
	float const uv(fract((val - c.d[dim][0])/window_spacing));
	return (uv > window_border && uv < 1.0f-window_border);
}

struct split_cube_t : public cube_t {
	float door_lo[2][2], door_hi[2][2]; // per {dim x dir}
	
	split_cube_t(cube_t const &c) : cube_t(c) {
		door_lo[0][0] = door_lo[0][1] = door_lo[1][0] = door_lo[1][1] = door_hi[0][0] = door_hi[0][1] = door_hi[1][0] = door_hi[1][1] = 0.0f;
	}
	bool bad_pos(float val, bool dim) const {
		for (unsigned d = 0; d < 2; ++d) { // check both dirs (wall end points)
			if (door_lo[dim][d] < door_hi[dim][d] && val > door_lo[dim][d] && val < door_hi[dim][d]) return 1;
		}
		return 0;
	}
};

unsigned calc_num_floors(cube_t const &c, float window_vspacing, float floor_thickness) {
	float const z_span(c.dz() - floor_thickness);
	assert(z_span > 0.0);
	unsigned const num_floors(round_fp(z_span/window_vspacing)); // round - no partial floors
	assert(num_floors <= 100); // sanity check
	return num_floors;
}

void subtract_cube_xy(cube_t const &c, cube_t const &r, cube_t *out) { // subtract r from c; ignores zvals
	assert(c.contains_cube_xy(r));
	for (unsigned i = 0; i < 4; ++i) {out[i] = c;}
	out[0].y2() = r.y1();
	out[1].y1() = r.y2();
	out[2].y1() = r.y1(); out[2].y2() = r.y2(); out[2].x2() = r.x1();
	out[3].y1() = r.y1(); out[3].y2() = r.y2(); out[3].x1() = r.x2();
}

void building_t::gen_interior(rand_gen_t &rgen, bool has_overlapping_cubes) { // Note: contained in building bcube, so no bcube update is needed

	if (!ADD_BUILDING_INTERIORS) return; // disabled
	if (world_mode != WMODE_INF_TERRAIN) return; // tiled terrain mode only
	if (!global_building_params.windows_enabled()) return; // no windows, can't assign floors and generate interior
	//if (has_overlapping_cubes) return; // overlapping cubes buildings are more difficult to handle
	if (!is_cube()) return; // only generate interiors for cube buildings for now
	building_mat_t const &mat(get_material());
	if (!mat.add_windows) return; // not a building type that has generated windows (skip office buildings with windows baked into textures)
	// defer this until the building is close to the player?
	interior.reset(new building_interior_t);
	float const window_vspacing(mat.get_floor_spacing());
	float const floor_thickness(FLOOR_THICK_VAL*window_vspacing), fc_thick(0.5*floor_thickness);
	float const doorway_width(0.5*window_vspacing), doorway_hwidth(0.5*doorway_width);
	float const wall_thick(0.5*floor_thickness), wall_half_thick(0.5*wall_thick), wall_edge_spacing(0.05*wall_thick), min_wall_len(4.0*doorway_width);
	float const wwf(global_building_params.get_window_width_fract()), window_border(0.5*(1.0 - wwf)); // (0.0, 1.0)
	// houses have at most two parts; exclude garage, shed, porch, porch support, etc.
	unsigned const num_parts(get_real_num_parts());
	vector<split_cube_t> to_split;
	uint64_t must_split[2] = {0,0};
	unsigned first_wall_to_split[2] = {0,0};
	// allocate space for all floors
	unsigned tot_num_floors(0), tot_num_stairwells(0), tot_num_landings(0); // num floor/ceiling cubes, not number of stories

	for (auto p = parts.begin(); p != (parts.begin() + num_parts); ++p) {
		bool const has_stairs(!is_house || p == parts.begin()); // assumes one set of stairs or elevator per part
		unsigned const cubes_per_floor(has_stairs ? 4 : 1); // account for stairwell cutouts
		unsigned const num_floors(calc_num_floors(*p, window_vspacing, floor_thickness));
		tot_num_floors     += cubes_per_floor*(num_floors - 1) + 1; // first floor has no cutout
		tot_num_stairwells += (has_stairs && num_floors > 1);
		tot_num_landings   += (has_stairs ? (num_floors - 1) : 0);
	}
	interior->ceilings.reserve(tot_num_floors);
	interior->floors  .reserve(tot_num_floors);
	interior->landings.reserve(tot_num_landings);
	interior->stairwells.reserve(tot_num_stairwells);
	
	// generate walls and floors for each part;
	// this will need to be modified to handle buildings that have overlapping parts, or skip those building types completely
	for (auto p = parts.begin(); p != (parts.begin() + num_parts); ++p) {
		unsigned const num_floors(calc_num_floors(*p, window_vspacing, floor_thickness));
		if (num_floors == 0) continue; // not enough space to add a floor (can this happen?)
		// for now, assume each part has the same XY bounds and can use the same floorplan; this means walls can span all floors and don't need to be duplicated for each floor
		vector3d const psz(p->get_size());
		bool const min_dim(psz.y < psz.x); // hall dim
		float const cube_width(psz[min_dim]);
		bool const first_part(p == parts.begin());
		bool const use_hallway(!is_house && (first_part || (p-1)->z1() < p->z1()) && (p+1 == parts.end() || (p+1)->z1() > p->z1()) && cube_width > 4.0*min_wall_len);
		unsigned const rooms_start(interior->rooms.size()), part_id(p - parts.begin());
		cube_t hall;

		if (use_hallway) {
			// building with rectangular slice (no adjacent exterior walls at this level), generate rows of offices
			int const num_windows   (get_num_windows_on_side(p->d[!min_dim][0], p->d[!min_dim][1]));
			int const num_windows_od(get_num_windows_on_side(p->d[ min_dim][0], p->d[ min_dim][1])); // other dim, for use in hallway width calculation
			int const windows_per_room((num_windows > 5) ? 2 : 1); // 1-2 windows per room
			int const num_rooms((num_windows+windows_per_room-1)/windows_per_room); // round up
			bool const partial_room((num_windows % windows_per_room) != 0); // an odd number of windows leaves a small room at the end
			assert(num_rooms >= 0 && num_rooms < 1000); // sanity check
			float const window_hspacing(psz[!min_dim]/num_windows), room_len(window_hspacing*windows_per_room);
			float const num_hall_windows((num_windows_od & 1) ? 1.4 : 1.8); // hall either contains 1 (odd) or 2 (even) windows, wider for single window case to make room for stairs
			float const hall_width(num_hall_windows*psz[min_dim]/num_windows_od);
			float const room_width(0.5f*(cube_width - hall_width)); // rooms are the same size on each side of the hallway
			float const hwall_extend(0.5f*(room_len - doorway_width - wall_thick));
			float const hall_wall_pos[2] = {(p->d[min_dim][0] + room_width), (p->d[min_dim][1] - room_width)};
			hallway_dim = !min_dim; // cache in building for later use
			vect_cube_t &room_walls(interior->walls[!min_dim]), &hall_walls(interior->walls[min_dim]);
			room_walls.reserve(2*(num_rooms-1));
			hall_walls.reserve(2*(num_rooms+1));
			cube_t rwall(*p); // copy from part; shared zvals, but X/Y will be overwritten per wall
			float const wall_pos(p->d[!min_dim][0] + room_len); // pos of first wall separating first from second rooms
			create_wall(rwall, !min_dim, wall_pos, fc_thick, wall_half_thick, wall_edge_spacing); // room walls

			for (int i = 0; i+1 < num_rooms; ++i) { // num_rooms-1 walls
				for (unsigned d = 0; d < 2; ++d) {
					room_walls.push_back(rwall);
					room_walls.back().d[min_dim][!d] = hall_wall_pos[d];
					cube_t hwall(room_walls.back());
					for (unsigned e = 0; e < 2; ++e) {hwall.d[ min_dim][e]  = hall_wall_pos[d] + (e ? 1.0f : -1.0f)*wall_half_thick;}
					for (unsigned e = 0; e < 2; ++e) {hwall.d[!min_dim][e] += (e ? 1.0f : -1.0f)*hwall_extend;}
					if (partial_room && i+2 == num_rooms) {hwall.d[!min_dim][1] -= 1.5*doorway_width;} // pull back a bit to make room for a doorway at the end of the hall
					hall_walls.push_back(hwall); // longer sections that form T-junctions with room walls
				}
				for (unsigned e = 0; e < 2; ++e) {rwall.d[!min_dim][e] += room_len;}
			} // for i
			for (unsigned s = 0; s < 2; ++s) { // add half length hall walls at each end of the hallway
				cube_t hwall(rwall); // copy to get correct zvals
				float const hwall_len((partial_room && s == 1) ? doorway_width : hwall_extend); // hwall for partial room at end is only length doorway_width
				hwall.d[!min_dim][ s] = p->d   [!min_dim][s] + (s ? -1.0f : 1.0f)*wall_edge_spacing; // end at the wall
				hwall.d[!min_dim][!s] = hwall.d[!min_dim][s] + (s ? -1.0f : 1.0f)*hwall_len; // end at first doorway

				for (unsigned d = 0; d < 2; ++d) {
					for (unsigned e = 0; e < 2; ++e) {hwall.d[ min_dim][e] = hall_wall_pos[d] + (e ? 1.0f : -1.0f)*wall_half_thick;}
					hall_walls.push_back(hwall);
				}
			} // for s
			// add rooms and doors
			interior->rooms.reserve(2*num_rooms + 1); // two rows of rooms + hallway
			interior->doors.reserve(2*num_rooms);
			float pos(p->d[!min_dim][0]);

			for (int i = 0; i < num_rooms; ++i) {
				float const wall_end(p->d[!min_dim][1]);
				bool const is_small_room((pos + room_len) > wall_end);
				float const next_pos(is_small_room ? wall_end : (pos + room_len)); // clamp to end of building to last row handle partial room)

				for (unsigned d = 0; d < 2; ++d) { // lo, hi
					cube_t c(*p); // copy zvals and exterior wall pos
					c.d[ min_dim][!d] = hall_wall_pos[d];
					c.d[!min_dim][ 0] = pos;
					c.d[!min_dim][ 1] = next_pos;
					add_room(c, part_id);
					interior->rooms.back().is_office = 1;
					cube_t door(c); // copy zvals and wall pos
					door.d[ min_dim][d] = hall_wall_pos[d]; // set to zero area at hallway
					door.d[!min_dim][0] += hwall_extend; // shrink to doorway width
					door.d[!min_dim][1] -= hwall_extend;
					interior->doors.push_back(door);
				}
				pos = next_pos;
			} // for i
			hall = *p;
			for (unsigned e = 0; e < 2; ++e) {hall.d[min_dim][e] = hall_wall_pos[e];}
			add_room(hall, part_id); // add hallway as room
			interior->rooms.back().is_hallway = interior->rooms.back().no_geom = 1;
			for (unsigned d = 0; d < 2; ++d) {first_wall_to_split[d] = interior->walls[d].size();} // don't split any walls added up to this point
		}
		else { // generate random walls using recursive 2D slices
			if (min(p->dx(), p->dy()) < 2.0*doorway_width) continue; // not enough space to add a room (chimney, porch support, garage, shed, etc.)
			assert(to_split.empty());
			to_split.emplace_back(*p); // seed room is entire part, no door
			float window_hspacing[2] = {0.0};
			
			if (first_part) { // reserve walls/rooms/doors - take a guess at the correct size
				for (unsigned d = 0; d < 2; ++d) {interior->walls[d].reserve(8*parts.size());}
				interior->rooms.reserve(8*parts.size()); // two rows of rooms + optional hallway
				interior->doors.reserve(4*parts.size());
			}
			for (unsigned d = 0; d < 2; ++d) {
				int const num_windows(get_num_windows_on_side(p->d[d][0], p->d[d][1]));
				window_hspacing[d] = psz[d]/num_windows;
			}
			while (!to_split.empty()) {
				split_cube_t c(to_split.back()); // Note: non-const because door_lo/door_hi is modified during T-junction insert
				to_split.pop_back();
				vector3d const csz(c.get_size());
				bool wall_dim(0); // which dim the room is split by
				if      (csz.y > min_wall_len && csz.x > 1.25*csz.y) {wall_dim = 0;} // split long room in x
				else if (csz.x > min_wall_len && csz.y > 1.25*csz.x) {wall_dim = 1;} // split long room in y
				else {wall_dim = rgen.rand_bool();} // choose a random split dim for nearly square rooms
				
				if (min(csz.x, csz.y) < min_wall_len) {
					add_room(c, part_id);
					continue; // not enough space to add a wall
				}
				float wall_pos(0.0);
				bool const on_edge(c.d[wall_dim][0] == p->d[wall_dim][0] || c.d[wall_dim][1] == p->d[wall_dim][1]); // at edge of the building - make sure walls don't intersect windows
				bool pos_valid(0);
				
				for (unsigned num = 0; num < 20; ++num) { // 20 tries to choose a wall pos that's not inside a window
					wall_pos = cube_rand_side_pos(c, wall_dim, 0.25, (1.5*doorway_width + wall_thick), rgen);
					if (on_edge && is_val_inside_window(*p, wall_dim, wall_pos, window_hspacing[wall_dim], window_border)) continue; // try a new wall_pos
					if (c.bad_pos(wall_pos, wall_dim)) continue; // intersects doorway from prev wall, try a new wall_pos
					pos_valid = 1; break; // done, keep wall_pos
				}
				if (!pos_valid) { // no valid pos, skip this split
					add_room(c, part_id);
					continue;
				}
				cube_t wall(c), wall2; // copy from cube; shared zvals, but X/Y will be overwritten per wall
				create_wall(wall, wall_dim, wall_pos, fc_thick, wall_half_thick, wall_edge_spacing);
				float const doorway_pos(cube_rand_side_pos(c, !wall_dim, 0.25, doorway_width, rgen));
				float const lo_pos(doorway_pos - doorway_hwidth), hi_pos(doorway_pos + doorway_hwidth);
				remove_section_from_cube_and_add_door(wall, wall2, lo_pos, hi_pos, !wall_dim, interior->doors);
				interior->walls[wall_dim].push_back(wall);
				interior->walls[wall_dim].push_back(wall2);
				bool const do_split(csz[wall_dim] > max(global_building_params.wall_split_thresh, 1.0f)*min_wall_len); // split into two smaller rooms

				for (unsigned d = 0; d < 2; ++d) { // still have space to split in other dim, add the two parts to the stack
					split_cube_t c_sub(c);
					c_sub.d[wall_dim][d] = wall.d[wall_dim][!d]; // clip to wall pos
					c_sub.door_lo[!wall_dim][d] = lo_pos - wall_half_thick; // set new door pos in this dim (keep door pos in other dim, if set)
					c_sub.door_hi[!wall_dim][d] = hi_pos + wall_half_thick;
					if (do_split) {to_split.push_back(c_sub);} else {add_room(c_sub, part_id);} // leaf case (unsplit), add a new room
				}
			} // end while()
			// insert walls to split up parts into rectangular rooms
			for (auto p2 = parts.begin(); p2 != parts.end(); ++p2) {
				if (p2 == p) continue; // skip self

				for (unsigned dim = 0; dim < 2; ++dim) {
					for (unsigned dir = 0; dir < 2; ++dir) {
						float const val(p->d[!dim][dir]);
						if (p2->d[!dim][!dir] != val) continue; // not adjacent
						if (p2->z1() >= p->z2() || p2->z2() <= p->z1()) continue; // no overlap in Z
						if (p2->d[dim][0] > p->d[dim][0] || p2->d[dim][1] < p->d[dim][1]) continue; // not contained in dim (don't have to worry about Z-shaped case)

						if (p2->d[dim][0] == p->d[dim][0] && p2->d[dim][1] == p->d[dim][1]) { // same xy values, must only vary in z
							if (p2->z2() < p->z2()) continue; // add wall only on one side (arbitrary)
						}
						cube_t wall;
						wall.z1() = max(p->z1(), p2->z1()) + fc_thick; // shared Z range
						wall.z2() = min(p->z2(), p2->z2()) - fc_thick;
						wall.d[ dim][0] = p->d[dim][0] + wall_edge_spacing; // shorter part side with slight offset
						wall.d[ dim][1] = p->d[dim][1] - wall_edge_spacing;
						wall.d[!dim][ dir] = val;
						wall.d[!dim][!dir] = val + (dir ? -1.0 : 1.0)*wall_thick;
						must_split[!dim] |= (1ULL << (interior->walls[!dim].size() & 63)); // flag this wall for extra splitting
						interior->walls[!dim].push_back(wall);
					} // for dir
				} // for dim
			} // for p2
		} // end wall placement
		add_ceilings_floors_stairs(rgen, *p, hall, num_floors, rooms_start, use_hallway, first_part);
	} // for p (parts)
	// attempt to cut extra doorways into long walls if there's space to produce a more connected floorplan
	for (unsigned d = 0; d < 2; ++d) { // x,y: dim in which the wall partitions the room (wall runs in dim !d)
		vect_cube_t &walls(interior->walls[d]);
		vect_cube_t const &perp_walls(interior->walls[!d]);

		// Note: iteration will include newly added all segments to recursively split long walls
		for (unsigned w = first_wall_to_split[d]; w < walls.size(); ++w) { // skip hallway walls
			bool pref_split(must_split[d] & (1ULL << (w & 63)));

			for (unsigned nsplits = 0; nsplits < 4; ++nsplits) { // at most 4 splits
				cube_t &wall(walls[w]); // take a reference here because a prev iteration push_back() may have invalidated it
				float const len(wall.get_sz_dim(!d)), min_split_len((pref_split ? 0.75 : 1.5)*min_wall_len);
				if (len < min_split_len) break; // not long enough to split - done
				// walls currently don't run along the inside of exterior building walls, so we don't need to handle that case yet
				bool was_split(0);

				for (unsigned ntries = 0; ntries < (pref_split ? 10U : 4U); ++ntries) { // choose random doorway positions and check against perp_walls for occlusion
					float const doorway_pos(cube_rand_side_pos(wall, !d, 0.2, 1.5*doorway_width, rgen));
					float const lo_pos(doorway_pos - doorway_hwidth), hi_pos(doorway_pos + doorway_hwidth);
					bool valid(1);

					for (auto p = (perp_walls.begin() + first_wall_to_split[!d]); p != perp_walls.end(); ++p) { // skip hallway walls
						if (p->z1() != wall.z1()) continue; // not the same zval/story
						if (p->d[!d][1] < lo_pos-wall_thick || p->d[!d][0] > hi_pos+wall_thick) continue; // no overlap with wall
						if (p->d[ d][1] > wall.d[d][0]-wall_thick && p->d[d][0] < wall.d[d][1]+wall_thick) {valid = 0; break;} // has perp intersection
					}
					if (valid && !pref_split) { // don't split walls into small segments that border the same two rooms on both sides (two doorways between the same pair of rooms)
						float const lo[2] = {wall.d[!d][0]-wall_thick, lo_pos}, hi[2] = {hi_pos, wall.d[!d][1]+wall_thick}; // ranges of the two split wall segments, grown a bit

						for (unsigned s = 0; s < 2; ++s) { // check both wall segments
							bool contained[2] = {0,0};

							for (unsigned e = 0; e < 2; ++e) { // check both directions from the wall
								for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {
									if (wall.d[d][e] < r->d[d][0] || wall.d[d][e] > r->d[d][1]) continue; // wall not inside room in dim d/dir e
									if (lo[s] > r->d[!d][0] && hi[s] < r->d[!d][1]) {contained[e] = 1; break;} // entire wall contained in span of room
								}
							}
							if (contained[0] && contained[1]) {valid = 0; break;} // wall seg contained in rooms on both sides => two doors in same wall between rooms => drop
						} // for s
					}
					if (!valid) continue;
					cube_t cand(wall);
					cand.d[!d][0] = lo_pos; cand.d[!d][1] = hi_pos;
					if (interior->is_blocked_by_stairs_or_elevator(wall, doorway_width)) continue; // stairs in the way, skip; should we assert !pref_split?
					cube_t wall2;
					remove_section_from_cube_and_add_door(wall, wall2, lo_pos, hi_pos, !d, interior->doors);
					walls.push_back(wall2); // Note: invalidates wall reference
					was_split = 1;
					break;
				} // for ntries
				if (!was_split) break; // no more splits
				pref_split = 0; // already split, no longer preferred
			} // for nsplits
		} // for w
	} // for d
	interior->finalize();
}

void building_t::add_ceilings_floors_stairs(rand_gen_t &rgen, cube_t const &part, cube_t const &hall, unsigned num_floors, unsigned rooms_start, bool use_hallway, bool first_part) {

	float const window_vspacing(get_material().get_floor_spacing());
	float const floor_thickness(FLOOR_THICK_VAL*window_vspacing), fc_thick(0.5*floor_thickness), doorway_width(0.5*window_vspacing);
	float const ewidth(1.5*doorway_width); // for elevators
	float z(part.z1());
	cube_t stairs_cut, elevator_cut;
	bool stairs_dim(0), add_elevator(0);

	// add stairwells and elevator shafts
	if (num_floors == 1) {} // no need for stairs or elevator
	else if (use_hallway) { // part is the hallway cube
		add_elevator = 1; //rgen.rand_bool();
		if (first_part) {interior->landings.reserve(add_elevator ? 1 : (num_floors-1));}
		assert(!interior->rooms.empty());
		room_t &room(interior->rooms.back()); // hallway is always the last room to be added
		bool const long_dim(hall.dx() < hall.dy());
		cube_t stairs(hall); // start as hallway

		if (add_elevator) {
			point center(room.get_cube_center());
			float const center_shift(0.125*room.get_sz_dim(long_dim)*(rgen.rand_bool() ? -1.0 : 1.0));
			center[long_dim] += center_shift; // make elevator off-center
			elevator_t elevator(room, long_dim, rgen.rand_bool(), rgen.rand_bool()); // elevator shaft
			elevator.x1() = center.x - 0.5*ewidth; elevator.x2() = center.x + 0.5*ewidth;
			elevator.y1() = center.y - 0.5*ewidth; elevator.y2() = center.y + 0.5*ewidth;
			interior->elevators.push_back(elevator);
			room.has_elevator = 1;
			elevator_cut      = elevator;
			for (unsigned d = 0; d < 2; ++d) {stairs.d[long_dim][d] -= center_shift;} // shift stairs in the opposite direction
		}
		// always add stairs
		for (unsigned dim = 0; dim < 2; ++dim) { // shrink in XY
			bool const is_step_dim(bool(dim) == long_dim); // same orientation as the hallway
			float shrink(stairs.get_sz_dim(dim) - (is_step_dim ? 4.0*doorway_width : 0.9*ewidth)); // set max size of stairs opening, slightly narrower than elevator
			stairs.d[dim][0] += 0.5*shrink; stairs.d[dim][1] -= 0.5*shrink; // centered in the hallway
		}
		room.has_stairs = 1;
		stairs_cut      = stairs;
	}
	else if (!is_house || first_part) { // only add stairs to first part of a house
		// add elevator half of the time to building parts, but not the first part (to guarantee we have at least one set of stairs)
		// it might not be possible to place an elevator a part with no interior rooms, but that should be okay, because some other part will still have stairs
		// do we need support for multiple floor cutouts stairs + elevator in this case as well?
		add_elevator = (!is_house && !first_part && rgen.rand_bool());
		unsigned const rooms_end(interior->rooms.size()), num_avail_rooms(rooms_end - rooms_start);
		assert(num_avail_rooms > 0); // must have added at least one room
		unsigned const rand_ix(rgen.rand()); // choose a random starting room to make into a stairwell

		for (unsigned n = 0; n < num_avail_rooms; ++n) { // try all available rooms starting with the selected one to see if we can fit a stairwell in any of them
			unsigned const stairs_room(rooms_start + (rand_ix + n)%num_avail_rooms);
			room_t &room(interior->rooms[stairs_room]);

			if (add_elevator) {
				if (min(room.dx(), room.dy()) < 2.0*ewidth) continue; // room is too small to place an elevator
				bool placed(0);

				for (unsigned y = 0; y < 2 && !placed; ++y) { // try all 4 corners
					for (unsigned x = 0; x < 2 && !placed; ++x) {
						// don't place elevators on building exteriors blocking windows or between parts where they would block doorways
						if (room.d[0][x] == part.d[0][x] || room.d[1][y] == part.d[1][y]) continue;
						bool const dim(rgen.rand_bool()), is_open(rgen.rand_bool());
						elevator_t elevator(room, dim, !(dim ? y : x), is_open); // elevator shaft
						elevator.d[0][!x] = elevator.d[0][x] + (x ? -ewidth : ewidth);
						elevator.d[1][!y] = elevator.d[1][y] + (y ? -ewidth : ewidth);
						elevator.expand_by_xy(-0.01*ewidth); // shrink to leave a small gap between the outer wall to prevent z-fighting
						if (interior->is_cube_close_to_doorway(elevator)) continue; // try again
						interior->elevators.push_back(elevator);
						elevator_cut = elevator;
						placed       = 1; // successfully placed
					} // for x
				} // for y
				if (!placed) continue; // try another room
				room.has_elevator = 1;
				room.no_geom      = 1;
			}
			else { // stairs
				cube_t cutout(room);
				cutout.expand_by_xy(-floor_thickness); // padding around walls
				float const dx(cutout.dx()), dy(cutout.dy()); // choose longer dim of high aspect ratio
				if      (dx > 1.2*dy) {stairs_dim = 0;}
				else if (dy > 1.2*dx) {stairs_dim = 1;}
				else {stairs_dim = rgen.rand_bool();} // close to square
				if (cutout.get_sz_dim(stairs_dim) < 4.0*doorway_width || cutout.get_sz_dim(!stairs_dim) < 3.0*doorway_width) continue; // not enough space for stairs

				for (unsigned dim = 0; dim < 2; ++dim) { // shrink in XY
					bool const is_step_dim(bool(dim) == stairs_dim);
					float shrink(cutout.get_sz_dim(dim) - (is_step_dim ? 4.0 : 1.2)*doorway_width); // set max size of stairs opening
					max_eq(shrink, 2.0f*doorway_width); // allow space for doors to open and player to enter/exit
					cutout.d[dim][0] += 0.5*shrink; cutout.d[dim][1] -= 0.5*shrink; // centered in the room

					if (!is_step_dim) { // see if we can push the stairs to the wall on one of the sides without blocking a doorway
						bool const first_dir(rgen.rand_bool());

						for (unsigned d = 0; d < 2; ++d) {
							bool const dir(bool(d) ^ first_dir);
							// if the room is on the edge of the part that's not on the building bcube exterior, then this room connects two parts and we need to place a door here later;
							// technically, it's more accurate to check for an adjacent part, but that's somewhat difficult to do here
							if (room.d[dim][dir] == part.d[dim][dir] && part.d[dim][dir] != bcube.d[dim][dir]) continue;
							cube_t cand(cutout);
							float const shift(0.95f*(cand.d[dim][dir] - room.d[dim][dir])); // negative if dir==1, add small gap to prevent z-fighting and FP accuracy asserts
							cand.d[dim][0] -= shift; cand.d[dim][1] -= shift; // close the gap - flush with the wall
							if (!interior->is_cube_close_to_doorway(cand)) {cutout = cand; break;} // keep if it's good
						}
					}
				} // for dim
				if (first_part) {interior->landings.reserve(add_elevator ? 1 : (num_floors-1));}
				assert(cutout.is_strictly_normalized());
				stairs_cut      = cutout;
				room.has_stairs = 1;
				//room.no_geom    = 1;
			}
			break; // success - done
		} // for n
	}
	// add ceilings and floors; we have num_floors+1 separators; the first is only a floor, and the last is only a ceiling
	cube_t C(part);
	C.z1() = z; C.z2() = z + fc_thick;
	unsigned const floors_start(interior->floors.size());
	interior->floors.push_back(C); // ground floor, full area
	z += window_vspacing; // move to next floor
	bool const has_stairs(!stairs_cut.is_all_zeros()), has_elevator(!elevator_cut.is_all_zeros());
	cube_t &first_cut(has_elevator ? elevator_cut : stairs_cut); // elevator is larger

	for (unsigned f = 1; f < num_floors; ++f, z += window_vspacing) { // skip first floor - draw pairs of floors and ceilings
		cube_t to_add[8];
		float const zc(z - fc_thick), zf(z + fc_thick);

		if (!has_stairs && !has_elevator) {to_add[0] = part;} // neither - add single cube
		else {
			subtract_cube_xy(part, first_cut, to_add);

			if (has_stairs && has_elevator) { // both
				bool found(0);

				for (unsigned n = 0; n < 4; ++n) { // find the cube where the stairs are placed
					if (!to_add[n].intersects_xy_no_adj(stairs_cut)) continue;
					subtract_cube_xy(to_add[n], stairs_cut, to_add+4); // append up to 4 more cubes
					to_add[n].set_to_zeros(); // this cube was replaced
					found = 1;
					break; // assume there is only one; it's up to the placer step to ensure this
				}
				assert(found);
			}
			if (has_stairs) { // add landings and stairwells
				landing_t landing(stairs_cut, 0);
				landing.z1() = zc; landing.z2() = zf;
				interior->landings.push_back(landing);
				if (f == 1) {interior->stairwells.push_back(stairs_cut);} // only add for first floor
			}
			if (has_elevator) {
				landing_t landing(elevator_cut, 1);
				landing.z1() = zc; landing.z2() = zf;
				interior->landings.push_back(landing);
			}
		}
		for (unsigned i = 0; i < 8; ++i) { // skip zero area cubes from stairs/elevator shafts along an exterior wall
			cube_t &c(to_add[i]);
			if (c.is_zero_area()) continue;
			c.z1() = zc; c.z2() = z;  interior->ceilings.push_back(c);
			c.z1() = z;  c.z2() = zf; interior->floors  .push_back(c);
			//c.z1() = zf; c.z2() = zc + window_vspacing; // add per-floor walls, door cutouts, etc. here
		}
	} // for f
	C.z1() = z - fc_thick; C.z2() = z;
	interior->ceilings.push_back(C); // roof ceiling, full area
	std::reverse(interior->floors.begin()+floors_start, interior->floors.end()); // order floors top to bottom to reduce overdraw when viewed from above
}

void building_t::add_room(cube_t const &room, unsigned part_id) {
	assert(interior);
	room_t r(room, part_id);
	cube_t const &part(parts[part_id]);
	for (unsigned d = 0; d < 4; ++d) {r.ext_sides |= (unsigned(room.d[d>>1][d&1] == part.d[d>>1][d&1]) << d);} // find exterior sides
	interior->rooms.push_back(r);
}

bool building_t::add_table_and_chairs(rand_gen_t &rgen, cube_t const &room, unsigned room_id, point const &place_pos, float rand_place_off, float tot_light_amt, bool is_lit) {

	// TODO_INT: use tot_light_amt as an approximation for ambient lighting due to sun/moon? Do we need per-object lighting colors?
	float const window_vspacing(get_window_vspace());
	vector3d const room_sz(room.get_size());
	vector<room_object_t> &objs(interior->room_geom->objs);
	point table_pos(place_pos);
	vector3d table_sz;
	for (unsigned d = 0; d < 2; ++d) {table_sz [d]  = 0.18*window_vspacing*(1.0 + rgen.rand_float());} // half size relative to window_vspacing
	for (unsigned d = 0; d < 2; ++d) {table_pos[d] += rand_place_off*room_sz[d]*rgen.rand_uniform(-1.0, 1.0);} // near the center of the room
	point llc(table_pos - table_sz), urc(table_pos + table_sz);
	llc.z = table_pos.z; // bottom
	urc.z = table_pos.z + 0.2*window_vspacing;
	cube_t table(llc, urc);
	if (!interior->is_valid_placement_for_room(table, room)) return 0; // check proximity to doors
	objs.emplace_back(table, TYPE_TABLE, room_id);
	if (is_lit) {objs.back().flags |= RO_FLAG_LIT;}
	float const chair_sz(0.1*window_vspacing); // half size

	// place some chairs around the table
	for (unsigned dim = 0; dim < 2; ++dim) {
		for (unsigned dir = 0; dir < 2; ++dir) {
			if (rgen.rand_bool()) continue; // 50% of the time
			point chair_pos(table_pos); // same starting center and z1
			chair_pos[dim] += (dir ? -1.0f : 1.0f)*(rgen.rand_uniform(-0.5, 1.2)*chair_sz + table_sz[dim]);
			cube_t chair(chair_pos, chair_pos);
			chair.z2() += 0.4*window_vspacing; // chair height
			chair.expand_by(vector3d(chair_sz, chair_sz, 0.0));
			if (!interior->is_valid_placement_for_room(chair, room)) continue; // check proximity to doors
			objs.emplace_back(chair, TYPE_CHAIR, room_id, dim, dir);
			if (is_lit) {objs.back().flags |= RO_FLAG_LIT;}
		} // for dir
	} // for dim
	return 1;
}

// Note: these three floats can be calculated from mat.get_floor_spacing(), but it's easier to change the constants if we just pass them in
void building_t::gen_room_details(rand_gen_t &rgen) {

	assert(interior);
	if (interior->room_geom) return; // already generated?
	//timer_t timer("Gen Room Details");
	interior->room_geom.reset(new building_room_geom_t);
	vector<room_object_t> &objs(interior->room_geom->objs);
	float const window_vspacing(get_window_vspace()), floor_thickness(FLOOR_THICK_VAL*window_vspacing), fc_thick(0.5*floor_thickness);
	interior->room_geom->obj_scale = window_vspacing; // used to scale room object textures
	unsigned tot_num_rooms(0);
	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {tot_num_rooms += calc_num_floors(*r, window_vspacing, floor_thickness);}
	objs.reserve(tot_num_rooms); // placeholder - there will be more than this many

	for (auto r = interior->rooms.begin(); r != interior->rooms.end(); ++r) {
		float const light_amt(r->get_light_amt());
		unsigned const num_floors(calc_num_floors(*r, window_vspacing, floor_thickness));
		unsigned const room_id(r - interior->rooms.begin());
		point room_center(r->get_cube_center());
		// determine light pos and size for this stack of rooms
		bool const light_dim(r->dx() < r->dy()); // longer room dim
		float const light_size((r->is_hallway ? 2.0 : (r->is_office ? 1.5 : 1.0))*floor_thickness); // use larger light for offices and hallways
		cube_t light;

		for (unsigned dim = 0; dim < 2; ++dim) {
			float const sz(((bool(dim) == light_dim) ? 2.2 : 1.0)*light_size);
			light.d[dim][0] = room_center[dim] - sz;
			light.d[dim][1] = room_center[dim] + sz;
		}
		bool const blocked_by_stairs(!r->is_hallway && interior->is_blocked_by_stairs_or_elevator(light, fc_thick));
		float z(r->z1());

		// place objects on each floor for this room
		for (unsigned f = 0; f < num_floors; ++f, z += window_vspacing) {
			room_center.z = z + fc_thick; // floor height
			bool const top_of_stairs(blocked_by_stairs && f+1 == num_floors);
			bool is_lit(0);

			if (!blocked_by_stairs || top_of_stairs) { // add a light to the center of the ceiling of this room if there's space (always for top of stairs)
				light.z2() = z + window_vspacing - fc_thick;
				light.z1() = light.z2() - 0.5*fc_thick;
				is_lit = (r->is_hallway || ((rgen.rand() & (top_of_stairs ? 3 : 1)) != 0)); // 50% of lights are on, 75% for top of stairs, 100% for hallways
				unsigned char flags(0);
				if (is_lit)        {flags |= RO_FLAG_LIT;}
				if (top_of_stairs) {flags |= RO_FLAG_TOS;}
				if (r->has_stairs) {flags |= RO_FLAG_RSTAIRS;}
				
				if (r->is_hallway) { // place a light on each side of the stairs, and also between stairs and elevator if there are both
					unsigned const num_lights((r->has_elevator && r->has_stairs) ? 3 : 2);
					float const offset(((num_lights == 3) ? 0.3 : 0.2)*r->get_sz_dim(light_dim)); // closer to the ends in the 3 lights case

					for (unsigned d = 0; d < num_lights; ++d) {
						float const delta((d == 2) ? 0.0 : (d ? -1.0 : 1.0)*offset); // last light is in the center
						cube_t hall_light(light);
						for (unsigned e = 0; e < 2; ++e) {hall_light.d[light_dim][e] += delta;}
						objs.emplace_back(hall_light, TYPE_LIGHT, room_id, light_dim, 0, flags); // dir=0 (unused)
					}
				}
				else { // normal room
					objs.emplace_back(light, TYPE_LIGHT, room_id, light_dim, 0, flags); // dir=0 (unused)
				}
				if (is_lit) {r->lit_by_floor |= (1ULL << (f&63));} // flag this floor as being lit (for up to 64 floors)
			} // end light placement
			if (r->no_geom) continue; // no other geometry for this room
			float tot_light_amt(light_amt);
			if (is_lit) {tot_light_amt += 100.0f*light_size*light_size/(r->dx()*r->dy());} // light surface area divided by room surface area with some fudge constant

			// place a table and maybe some chairs near the center of the room 95% of the time if it's not a hallway
			if (rgen.rand_float() < 0.95) {add_table_and_chairs(rgen, *r, room_id, room_center, 0.1, tot_light_amt, is_lit);}
			//if (z == bcube.z1()) {} // any special logic that goes on the first floor is here
		} // for f
	} // for r
	add_stairs_and_elevators(rgen);
	objs.shrink_to_fit();
}

void building_t::add_stairs_and_elevators(rand_gen_t &rgen) {

	unsigned const num_stairs = 12;
	float const window_vspacing(get_window_vspace()), floor_thickness(FLOOR_THICK_VAL*window_vspacing);
	float const stair_dz(window_vspacing/(num_stairs+1)), stair_height(stair_dz + floor_thickness);
	bool const dir(rgen.rand_bool()); // same for every floor, could alternate for stairwells if we were tracking it
	vector<room_object_t> &objs(interior->room_geom->objs);

	for (auto i = interior->landings.begin(); i != interior->landings.end(); ++i) {
		if (i->for_elevator) continue; // for elevator, not stairs
		bool const dim(i->dx() < i->dy()); // longer dim
		float const tot_len(i->get_sz_dim(dim)), step_len((dir ? 1.0 : -1.0)*tot_len/num_stairs), floor_z(i->z2() - window_vspacing);
		float z(floor_z - floor_thickness), pos(i->d[dim][!dir]);
		cube_t stair(*i);

		for (unsigned n = 0; n < num_stairs; ++n, z += stair_dz, pos += step_len) {
			stair.d[dim][!dir] = pos; stair.d[dim][dir] = pos + step_len;
			stair.z1() = max(floor_z, z); // don't go below the floor
			stair.z2() = z + stair_height;
			objs.emplace_back(stair, TYPE_STAIR, 0, dim, dir); // Note: room_id=0, not tracked, unused
		}
	} // for i
	for (auto i = interior->elevators.begin(); i != interior->elevators.end(); ++i) {
		// add any internal elevator parts with type=TYPE_ELEVATOR here
	}
}

void building_t::add_room_lights(vector3d const &xlate, unsigned building_id, bool camera_in_building, cube_t &lights_bcube) const {

	if (!has_room_geom()) return; // error?
	vector<room_object_t> &objs(interior->room_geom->objs);
	float const window_vspacing(get_window_vspace()), camera_z(camera_pdu.pos.z - xlate.z);

	for (auto i = objs.begin(); i != objs.end(); ++i) {
		if (i->type != TYPE_LIGHT || !(i->flags & RO_FLAG_LIT)) continue; // not a light, or light not on
		point const lpos(i->get_cube_center()); // centered in the light fixture
		if (!lights_bcube.contains_pt_xy(lpos)) continue; // not contained within the light volume
		point const cs_lpos(lpos + xlate); // camera space
		float const floor_z(i->z2() - window_vspacing), ceil_z(i->z2());
		bool const floor_is_above(camera_z < floor_z), floor_is_below(camera_z > ceil_z);
		bool const stairs_light(i->flags & (RO_FLAG_TOS | RO_FLAG_RSTAIRS));
		assert(i->room_id < interior->rooms.size());
		room_t const &room(interior->rooms[i->room_id]);
		
		if (floor_is_above || floor_is_below) { // light is on a different floor from the camera
			if (camera_in_building) { // player is on a different floor and can't see a light from the floor above/below
				if (!stairs_light) continue; // camera in building and on wrong floor, don't add light
				if (camera_z < (i->z2() - 2.0*window_vspacing) || camera_z > (i->z2() + window_vspacing)) continue; // light is on the stairs, add if one floor above/below
			}
			else { // camera outside the building
				float const xy_dist(p2p_dist_xy(camera_pdu.pos, cs_lpos));
				if ((camera_z - lpos.z) > 2.0f*xy_dist || (lpos.z - camera_z) > 0.5f*xy_dist) continue; // light viewed at too high an angle
				// is it better to check if light half sphere is occluded by the floor above/below?
				assert(room.part_id < parts.size());
				cube_t const &part(parts[room.part_id]);
				bool visible[2] = {0};

				for (unsigned d = 0; d < 2; ++d) { // for each dim
					bool const dir(camera_pdu.pos[d] > cs_lpos[d]);
					if ((camera_pdu.pos[d] > part.d[d][dir]) ^ dir) continue; // camera not on the outside face of the part containing this room, so can't see through any windows
					visible[d] = (room.ext_sides & (1 << (2*d + dir)));
				}
				if (!visible[0] && !visible[1]) continue; // room is not on the exterior of the building on either side facing the camera
			}
		} // end camera on different floor case
		float const light_radius(10.0*max(i->dx(), i->dy())), cull_radius(0.95*light_radius);
		if (!camera_pdu.sphere_visible_test(cs_lpos, cull_radius)) continue; // VFC
		// check visibility of bcube of light sphere clipped to building bcube; this excludes lights behind the camera and improves shadow map assignment quality
		cube_t clipped_bc; // in building space
		clipped_bc.set_from_sphere(lpos, cull_radius);
		clipped_bc.intersect_with_cube(bcube);
		if (!stairs_light) {clipped_bc.z1() = floor_z; clipped_bc.z2() = ceil_z;} // clip zval to current floor if light not in a room with stairs or elevator
		if (!camera_pdu.cube_visible(clipped_bc + xlate)) continue; // VFC
		// update lights_bcube and add light(s)
		min_eq(lights_bcube.z1(), (lpos.z - light_radius));
		max_eq(lights_bcube.z2(), (lpos.z + 0.1f*light_radius)); // pointed down - don't extend as far up
		float const bwidth = 0.25; // as close to 180 degree FOV as we can get without shadow clipping
		dl_sources.emplace_back(light_radius, lpos, lpos, WHITE, 0, -plus_z, bwidth); // points down, white for now
		dl_sources.back().set_building_id(building_id);

		if (camera_in_building && !room.is_hallway) { // only when the player is inside a building and can't see the light bleeding through the floor; not for hallways
			// add a smaller unshadowed light with near 180 deg FOV to illuminate the ceiling and other areas as cheap indirect lighting
			dl_sources.emplace_back((room.is_office ? 0.3 : 0.5)*light_radius, lpos, lpos, WHITE, 0, -plus_z, 0.4);
			dl_sources.back().set_building_id(building_id);
			dl_sources.back().disable_shadows();
		}
	} // for i
}

void building_t::gen_and_draw_room_geom(shader_t &s, unsigned building_ix, bool shadow_only) {
	if (!interior) return;
	rand_gen_t rgen;
	rgen.set_state(building_ix, parts.size()); // set to something canonical per building
	if (!is_rotated()) {gen_room_details(rgen);} // generate so that we can draw it; doesn't work with rotated buildings
	if (interior->room_geom) {interior->room_geom->draw(s, shadow_only);}
}

void building_t::clear_room_geom() {
	if (!has_room_geom()) return;
	interior->room_geom->clear(); // free VBO data before deleting the room_geom object
	interior->room_geom.reset();
}

void building_t::update_stats(building_stats_t &s) const { // calculate all of the counts that are easy to get
	++s.nbuildings;
	s.nparts   += parts.size();
	s.ndetails += details.size();
	s.ntquads  += roof_tquads.size();
	s.ndoors   += doors.size();
	if (!interior) return;
	++s.ninterior;
	s.nrooms  += interior->rooms.size();
	s.nceils  += interior->ceilings.size();
	s.nfloors += interior->floors.size();
	s.nwalls  += interior->walls[0].size() + interior->walls[1].size();
	s.ndoors  += interior->doors.size(); // I guess these also count as doors?
	if (!interior->room_geom) return;
	++s.nrgeom;
	s.nobjs  += interior->room_geom->objs.size();
	s.nverts += interior->room_geom->get_num_verts();
}

float room_t::get_light_amt() const { // Note: not normalized to 1.0
	float ext_perim(0.0);

	for (unsigned d = 0; d < 4; ++d) {
		if (ext_sides & (1<<d)) {ext_perim += get_sz_dim(d>>1);} // add length of each exterior side, assuming it has windows
	}
	return ext_perim/(dx()*dy()); // light per square meter = exterior perimeter over area
}

bool building_interior_t::is_cube_close_to_doorway(cube_t const &c, float dmin) const { // ignores zvals
	for (auto i = doors.begin(); i != doors.end(); ++i) {
		bool const dim(i->dy() < i->dx());
		if (c.d[!dim][0] > i->d[!dim][1] || c.d[!dim][1] < i->d[!dim][0]) continue; // no overlap in !dim
		float const min_dist((dmin == 0.0f) ? i->get_sz_dim(!dim) : dmin); // if dmin==0, use door width (so that door has space to open)
		if (c.d[dim][0] < i->d[dim][1]+min_dist && c.d[dim][1] > i->d[dim][0]-min_dist) return 1; // within min_dist
	}
	return 0;
}
bool building_interior_t::is_blocked_by_stairs_or_elevator(cube_t const &c, float dmin) const {
	return (has_bcube_int_xy(c, stairwells, dmin) || has_bcube_int_xy(c, elevators, dmin));
}
bool building_interior_t::is_valid_placement_for_room(cube_t const &c, cube_t const &room, float dmin) const {
	cube_t place_area(room);
	if (dmin != 0.0f) {place_area.expand_by_xy(-dmin);} // shrink by dmin
	if (!place_area.contains_cube_xy(c))   return 0; // not contained in interior part of the room
	if (is_cube_close_to_doorway(c, dmin)) return 0; // too close to a doorway
	if (is_blocked_by_stairs_or_elevator(c, dmin)) return 0; // faster to check only one per stairwell, but then we need to store another vector?
	return 1;
}

void building_interior_t::finalize() {
	remove_excess_cap(floors);
	remove_excess_cap(ceilings);
	remove_excess_cap(rooms);
	remove_excess_cap(doors);
	remove_excess_cap(landings);
	remove_excess_cap(stairwells);
	remove_excess_cap(elevators);
	for (unsigned d = 0; d < 2; ++d) {remove_excess_cap(walls[d]);}
}

colorRGBA const WOOD_COLOR(0.9, 0.7, 0.5); // light brown, multiplies wood texture color

// skip_faces: 1=Z1, 2=Z2, 4=Y1, 8=Y2, 16=X1, 32=X2 to match CSG cube flags
void rgeom_mat_t::add_cube_to_verts(cube_t const &c, colorRGBA const &color, unsigned skip_faces) {
	vertex_t v;
	v.set_c4(color);

	// Note: stolen from draw_cube() with tex coord logic, back face culling, etc. removed
	for (unsigned i = 0; i < 3; ++i) { // iterate over dimensions
		unsigned const d[2] = {i, ((i+1)%3)}, n((i+2)%3);

		for (unsigned j = 0; j < 2; ++j) { // iterate over opposing sides, min then max
			if (skip_faces & (1 << (2*(2-n) + j))) continue; // skip this face
			v.set_ortho_norm(n, j);
			v.v[n] = c.d[n][j];

			for (unsigned s1 = 0; s1 < 2; ++s1) {
				v.v[d[1]] = c.d[d[1]][s1];
				v.t[0] = tex.tscale_x*v.v[d[1]];

				for (unsigned k = 0; k < 2; ++k) { // iterate over vertices
					v.v[d[0]] = c.d[d[0]][k^j^s1^1]; // need to orient the vertices differently for each side
					v.t[1] = tex.tscale_y*v.v[d[0]];
					verts.push_back(v);
				}
			}
		} // for j
	} // for i
}

void rgeom_mat_t::create_vbo() {
	vbo.create_and_upload(verts);
	num_verts = verts.size();
	clear_container(verts); // no longer needed
}

void rgeom_mat_t::draw(shader_t &s, bool shadow_only) {
	if (shadow_only && tex.emissive) return; // assume this is a light source and shouldn't produce shadows
	assert(vbo.vbo_valid());
	assert(num_verts > 0);
	if (!shadow_only) {tex.set_gl(s);} // ignores texture scale for now
	vbo.pre_render();
	vertex_t::set_vbo_arrays();
	draw_quads_as_tris(num_verts);
	tex.unset_gl(s);
}

void building_room_geom_t::add_tc_legs(cube_t const &c, colorRGBA const &color, float width, float tscale) {
	rgeom_mat_t &mat(get_wood_material(tscale));

	for (unsigned y = 0; y < 2; ++y) {
		for (unsigned x = 0; x < 2; ++x) {
			cube_t leg(c);
			leg.d[0][x] += (1.0f - width)*(x ? -1.0f : 1.0f)*c.dx();
			leg.d[1][y] += (1.0f - width)*(y ? -1.0f : 1.0f)*c.dy();
			mat.add_cube_to_verts(leg, color, (EF_Z1 | EF_Z2)); // skip top and bottom faces
		}
	}
}

void building_room_geom_t::add_table(room_object_t const &c, float tscale) { // 6 quads for top + 4 quads per leg = 22 quads = 88 verts
	cube_t top(c), legs_bcube(c);
	top.z1() += 0.85*c.dz(); // 15% of height
	legs_bcube.z2() = top.z1();
	// TODO_INT: different lighting if (c.flags & RO_FLAG_LIT)? Applies to table and chairs
	get_wood_material(tscale).add_cube_to_verts(top, WOOD_COLOR); // all faces drawn
	add_tc_legs(legs_bcube, WOOD_COLOR, 0.08, tscale);
}

void building_room_geom_t::add_chair(room_object_t const &c, float tscale) { // 6 quads for seat + 5 quads for back + 4 quads per leg = 27 quads = 108 verts
	float const height(c.dz());
	cube_t seat(c), back(c), legs_bcube(c);
	seat.z1() += 0.32*height;
	seat.z2()  = back.z1() = seat.z1() + 0.07*height;
	legs_bcube.z2() = seat.z1();
	back.d[c.dim][c.dir] += 0.88f*(c.dir ? -1.0f : 1.0f)*c.get_sz_dim(c.dim);
	get_material(tid_nm_pair_t(MARBLE_TEX, 1.2*tscale)).add_cube_to_verts(seat, colorRGBA(0.2, 0.2, 1.0)); // light blue; all faces drawn
	get_wood_material(tscale).add_cube_to_verts(back, WOOD_COLOR, EF_Z1); // skip bottom face
	add_tc_legs(legs_bcube, WOOD_COLOR, 0.15, tscale);
}

void building_room_geom_t::add_stair(room_object_t const &c, float tscale) {
	get_material(tid_nm_pair_t(MARBLE_TEX, 1.5*tscale)).add_cube_to_verts(c, colorRGBA(0.85, 0.85, 0.85)); // all faces drawn
}

void building_room_geom_t::add_elevator(room_object_t const &c, float tscale) {
	assert(0); // not yet implemented
}

void building_room_geom_t::add_light(room_object_t const &c, float tscale) {
	// Note: need to use a different texture (or -1) for is_on because emissive flag alone does not cause a material change
	bool const is_on((c.flags & RO_FLAG_LIT) != 0);
	tid_nm_pair_t tp((is_on ? (int)WHITE_TEX : (int)PLASTER_TEX), tscale);
	tp.emissive = is_on;
	get_material(tp).add_cube_to_verts(c, WHITE, EF_Z2); // white, untextured, skip top face
}

void building_room_geom_t::clear() {
	for (auto m = materials.begin(); m != materials.end(); ++m) {m->clear();}
	materials.clear();
}

unsigned building_room_geom_t::get_num_verts() const {
	unsigned num_verts(0);
	for (auto m = materials.begin(); m != materials.end(); ++m) {num_verts += m->num_verts;}
	return num_verts;
}

rgeom_mat_t &building_room_geom_t::get_material(tid_nm_pair_t &tex) {
	// for now we do a simple linear search because there shouldn't be too many unique materials
	for (auto m = materials.begin(); m != materials.end(); ++m) {
		if (m->tex == tex) {return *m;}
	}
	materials.emplace_back(tex); // not found, add a new material
	return materials.back();
}
rgeom_mat_t &building_room_geom_t::get_wood_material(float tscale) {
	return get_material(tid_nm_pair_t(WOOD2_TEX, tscale)); // hard-coded for common material
}

void building_room_geom_t::create_vbos() {
	if (empty()) return; // no geom
	float const tscale(2.0/obj_scale);

	for (auto i = objs.begin(); i != objs.end(); ++i) {
		assert(i->is_strictly_normalized());
		switch (i->type) {
		case TYPE_NONE:  assert(0); // not supported
		case TYPE_TABLE: add_table(*i, tscale); break;
		case TYPE_CHAIR: add_chair(*i, tscale); break;
		case TYPE_STAIR: add_stair(*i, tscale); break;
		case TYPE_ELEVATOR: add_elevator(*i, tscale); break;
		case TYPE_LIGHT: add_light(*i, tscale); break; // light fixture
		default: assert(0); // undefined type
		}
	} // for i
	// Note: verts are temporary, but cubes are likely needed for things such as collision detection with the player (if it ever gets implemented)
	for (auto m = materials.begin(); m != materials.end(); ++m) {m->create_vbo();}
}

void building_room_geom_t::draw(shader_t &s, bool shadow_only) { // non-const because it creates the VBO
	if (empty()) return; // no geom
	if (materials.empty()) {create_vbos();} // create materials if needed
	for (auto m = materials.begin(); m != materials.end(); ++m) {m->draw(s, shadow_only);}
	vbo_wrap_t::post_render();
}

