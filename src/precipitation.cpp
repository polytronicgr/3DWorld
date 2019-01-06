// 3D World - Precipitation Physics and Rendering (Rain and Snow)
// by Frank Gennari
// 10/24/12
#include "3DWorld.h"
#include "physics_objects.h"
#include "draw_utils.h"
#include "shaders.h"
#include "mesh.h"


float const TT_PRECIP_DIST  = 20.0;
float const WATER_PART_DIST = 1.0;

extern bool begin_motion;
extern int animate2, display_mode, camera_coll_id, precip_mode, DISABLE_WATER;
extern float temperature, fticks, zmin, water_plane_z, brightness, XY_SCENE_SIZE;
extern vector3d wind;
extern int coll_id[];
extern obj_group obj_groups[];


template <unsigned VERTS_PER_PRIM> class precip_manager_t {
protected:
	typedef vert_wrap_t vert_type_t;
	vector<vert_type_t> verts;
	rand_gen_t rgen; // modified in update logic
	float prev_zmin, cur_zmin, prev_zmax, cur_zmax, precip_dist;
	bool check_water_coll, check_mesh_coll, check_cobj_coll;

public:
	precip_manager_t() : prev_zmin(get_zmin()), cur_zmin(prev_zmin), prev_zmax(get_zmax()), cur_zmax(prev_zmax),
		check_water_coll(1), check_mesh_coll(1), check_cobj_coll(1) {}
	virtual ~precip_manager_t() {}
	void clear () {verts.clear();}
	bool empty () const {return verts.empty();}
	size_t size() const {return verts.size();}
	virtual float get_zmin() const {return ((world_mode == WMODE_GROUND) ? zbottom : get_tiled_terrain_water_level());}
	virtual float get_zmax() const {return get_cloud_zmax();}
	virtual size_t get_num_precip() {return 700*get_precip_rate();} // similar to precip max objects
	bool in_range(point const &pos) const {return dist_xy_less_than(pos, get_camera_pos(), precip_dist);}
	vector3d get_velocity(float vz) const {return fticks*(0.02*wind + vector3d(0.0, 0.0, vz));}
	
	void pre_update() {
		cur_zmin = get_zmin();
		cur_zmax = get_zmax();
		if (cur_zmin >= cur_zmax) {clear(); return;} // invalid range (water particles with no water?)

		// if zmin or zmax changes by more than some amount, then clear and regen point z-values so that rain/snow stays uniformly spaced in z
		if (fabs(prev_zmin - cur_zmin) > 0.05*(get_zmax() - cur_zmin) || fabs(prev_zmax - cur_zmax) > 0.25*(get_zmax() - cur_zmin)) {
			clear();
			prev_zmin = cur_zmin;
			prev_zmax = cur_zmax;
		}
		check_size();
		precip_dist = ((world_mode == WMODE_GROUND) ? XY_SCENE_SIZE : TT_PRECIP_DIST);
		//cout << "num: " << get_num_precip() << endl; // 28K .... 142K
	}
	point gen_pt(float zval) {
		point const camera(get_camera_pos());

		while (1) {
			vector3d const off(precip_dist*rgen.signed_rand_float(), precip_dist*rgen.signed_rand_float(), zval);
			if (off.x*off.x + off.y*off.y < precip_dist*precip_dist) {return (vector3d(camera.x, camera.y, 0.0) + off);}
		}
		return zero_vector; // never gets here
	}
	bool check_splash_dist(point const &pos) const {
		point const camera(get_camera_pos());
		return (pos.z < camera.z && dist_less_than(camera, pos, 5.0)); // skip splashes above the camera (assuming the surface points up)
	}
	void maybe_add_rain_splash(point const &pos, point const &bot_pos, float z_int, deque<sphere_t> &splashes, int x, int y, bool in_water) {
		float const t((z_int - pos.z)/(bot_pos.z - pos.z));
		point const cpos(pos + (bot_pos - pos)*t);
		if (!camera_pdu.point_visible_test(cpos)) return;
		if (check_splash_dist(cpos)) {splashes.push_back(sphere_t(cpos, 1.0));}
		if (in_water && (rgen.rand() & 1)) {add_splash(cpos, x, y, 0.5, 0.01, 0, zero_vector, 0);} // 50% of the time; no droplets
	}
	bool is_bot_pos_valid(point &pos, point const &bot_pos, deque<sphere_t> *splashes=nullptr) {
		if (world_mode != WMODE_GROUND) return 1;
		// check bottom of raindrop/snow below the mesh or top surface cobjs (even if just created)
		if (pos.z > max(ztop, czmax))   return 1; // above mesh and cobjs, no collision possible
		if (!is_over_mesh(pos))         return 1; // outside the simulation region, no collision possible
		int const x(get_xpos(bot_pos.x)), y(get_ypos(bot_pos.y));
		if (point_outside_mesh(x, y))   return 1;
			
		if (check_water_coll && !DISABLE_WATER && (display_mode & 0x04) && pos.z < water_matrix[y][x]) { // water collision
			if (splashes != nullptr && (rgen.rand() & 1)) {maybe_add_rain_splash(pos, bot_pos, water_matrix[y][x], *splashes, x, y, 1);} // 50% of the time
			return 0;
		}
		else if (check_mesh_coll && pos.z < mesh_height[y][x]) { // mesh collision
			if (splashes != nullptr) {maybe_add_rain_splash(pos, bot_pos, mesh_height[y][x], *splashes, x, y, 0);} // line_intersect_mesh(pos, bot_pos, cpos);
			return 0;
		}
		else if (check_cobj_coll && bot_pos.z < v_collision_matrix[y][x].zmax) { // possible cobj collision
			if (splashes != nullptr && check_splash_dist(bot_pos)) {
				point cpos;
				vector3d cnorm;
				int cindex;
				if (camera_pdu.point_visible_test(bot_pos) && check_coll_line_exact(pos, bot_pos, cpos, cnorm, cindex, 0.0, camera_coll_id)) {splashes->push_back(sphere_t(cpos, 1.0));}
			}
			return 0;
		}
		return 1;
	}
	void check_pos(point &pos, point const &bot_pos, deque<sphere_t> *splashes=nullptr) {
		if (pos == all_zeros) { // initial location
			vector3d const bot_delta(bot_pos - pos);
			for (unsigned attempt = 0; attempt < 16; ++attempt) { // make 16 attempts at choosing a valid starting z-value
				pos = gen_pt(rgen.rand_uniform(cur_zmin, cur_zmax));
				if (is_bot_pos_valid(pos, pos+bot_delta, nullptr)) break;
			}
		}
		else if (pos.z < cur_zmin)                          {pos = gen_pt(cur_zmax);} // start again near the top
		else if (!in_range(pos))                            {pos = gen_pt(pos.z   );} // move inside the range
		else if (!is_bot_pos_valid(pos, bot_pos, splashes)) {pos = gen_pt(cur_zmax);} // start again near the top
	}
	void check_size() {verts.resize(VERTS_PER_PRIM*get_num_precip(), all_zeros);}
};


class rain_manager_t : public precip_manager_t<2> {

	deque<sphere_t> splashes;
	quad_batch_draw splash_qbd;
	line_tquad_draw_t drawer;
	colorRGBA color;

	void gen_draw_data() {
		drawer.clear();
		splash_qbd.clear();
		get_avg_sky_color(color);
		color.alpha = 0.2;
		point const camera(get_camera_pos());
		float const width = 0.002;
		float const splash_size = 0.004; // 2x-8x rain line diameter

		for (unsigned i = 0; i < verts.size(); i += 2) { // iterate in pairs (0.07ms for default rain intensity)
			if (dist_less_than(verts[i].v, camera, 0.5) && (camera_pdu.point_visible_test(verts[i].v) || camera_pdu.point_visible_test(verts[i+1].v))) {
				drawer.add_line_as_tris(verts[i].v, verts[i+1].v, width, width, color, color);
			}
		}
		// 0.08ms for default rain intensity
		for (auto i = splashes.begin(); i != splashes.end(); ++i) { // normal always faces up;
			float const sz(i->radius*splash_size), alpha(0.75*(4.0 - i->radius)/3.0); // size increases with radius/time; alpha decreases with radius/time
			splash_qbd.add_billboard(i->pos, camera, up_vector, colorRGBA(0.8, 0.9, 1.0, alpha), sz, sz, tex_range_t(), 0, &plus_z);
		}
	}
public:
	void update() {
		//timer_t timer("Rain Update"); // 0.64ms for default rain intensity / 2.66ms for 5x rain
		pre_update();
		vector3d const v(get_velocity(-0.2)), vinc(v*(0.1/verts.size())), dir(0.1*v.get_norm()); // length is 0.1
		vector3d vcur(v);
		while (!splashes.empty() && splashes.front().radius > 4.0) {splashes.pop_front();} // remove old splashes from the front
		if (animate2) {for (auto i = splashes.begin(); i != splashes.end(); ++i) {i->radius += 0.2*fticks;}}
		deque<sphere_t> *sv(begin_motion ? &splashes : nullptr);

//#pragma omp parallel for schedule(static,64) num_threads(2) // not valid for splashes, and actually slower for light rain
		for (int i = 0; i < (int)verts.size(); i += 2) { // iterate in pairs
			check_pos(verts[i].v, verts[i+1].v, sv);
			if (animate2) {verts[i].v += vcur; vcur += vinc;}
			verts[i+1].v = verts[i].v + dir;
		}
		gen_draw_data();
	}
	void render() const { // partially transparent
		if (empty()) return;
		//RESET_TIME;
		assert(!(size() & 1));
		enable_blend(); // split into point smooth and blend?
		shader_t s;
		s.begin_color_only_shader(color);
		draw_verts(verts, GL_LINES); // 0.08ms for default rain intensity
		glDepthMask(GL_FALSE); // disable depth writing
		drawer.draw(); // draw nearby raindrops as triangles (0.02ms for default rain intensity)
		s.end_shader();
		disable_blend();
		//PRINT_TIME("Rain Draw"); // similar to update time

		if (!splashes.empty()) { // 0.08ms for default rain intensity
			ensure_filled_polygons();
			enable_blend();
			shader_t s;
			setup_smoke_shaders(s, 0.01, 0, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0.0, 0.0, 0, 0, 1, 0); // disable rain/snow effects on splashes
			select_texture(FLARE2_TEX);
			splash_qbd.draw();
			s.end_shader();
			disable_blend();
			reset_fill_mode();
			//PRINT_TIME("Rain Draw + Splashes"); // 25-50% longer
		}
		glDepthMask(GL_TRUE);
	}
	void clear() {precip_manager_t<2>::clear(); splashes.clear(); drawer.clear(); splash_qbd.clear();}
};


class snow_manager_t : public precip_manager_t<1> {
	point_sprite_drawer psd;

	void gen_draw_data() {
		psd.clear();
		psd.reserve_pts(size());
		colorRGBA const color(WHITE*((world_mode == WMODE_GROUND) ? 1.5 : 1.0)*brightness); // constant
		color_wrapper const cw(color);
		for (vector<vert_type_t>::const_iterator i = verts.begin(); i != verts.end(); ++i) {psd.add_pt(vert_color(i->v, cw));}
	}
public:
	void update() {
		//timer_t timer("Snow Update");
		pre_update();
		float const vmult(0.1/verts.size());
		vector3d const v(get_velocity(-0.02)), v_step(vmult*v);
		vector3d v_add(v);

		for (unsigned i = 0; i < verts.size(); ++i) {
			check_pos(verts[i].v, verts[i].v);
			if (animate2) {verts[i].v += v_add; v_add += v_step;}
		}
		gen_draw_data();
	}
	void render() const {psd.draw(WHITE_TEX, 1.0);} // unblended pixels
	void clear() {precip_manager_t<1>::clear(); psd.clear();}
};


class uw_particle_manager_t : public precip_manager_t<1> { // underwater particles
	float terrain_zmin;
	point_sprite_drawer psd;
	vector<vector3d> velocity;

	void gen_draw_data() {
		psd.clear();
		psd.reserve_pts(size());
		float const cscale(1.0/WATER_PART_DIST);
		point const camera(get_camera_pos());
		colorRGBA base_color(WHITE);
		water_color_atten_at_pos(base_color, camera);

		for (vector<vert_type_t>::iterator i = verts.begin(); i != verts.end(); ++i) {
			colorRGBA color(base_color);
			color.A -= cscale*p2p_dist(camera, i->v);
			if (color.A <= 0.0) {i->v = gen_pt(i->v.z); continue;} // note: should be in check_pos()
			psd.add_pt(vert_color(i->v, color));
		}
	}
	virtual float get_zmin() const  {return max(terrain_zmin,  (get_camera_pos().z - WATER_PART_DIST));}
	virtual float get_zmax() const  {return min(water_plane_z, (get_camera_pos().z + WATER_PART_DIST));}
	virtual size_t get_num_precip() {return 150*get_precip_rate();}
public:
	uw_particle_manager_t() : terrain_zmin(0.0) {check_water_coll = 0;}

	void update(float terrain_zmin_) {
		terrain_zmin = terrain_zmin_;
		pre_update();
		precip_dist = WATER_PART_DIST;
		unsigned const vsz(velocity.size());
		velocity.resize(verts.size());
		for (unsigned i = vsz; i < velocity.size(); ++i) {velocity[i] = rgen.signed_rand_vector(0.0002);} // generate velocities if needed

		for (unsigned i = 0; i < verts.size(); ++i) {
			check_pos(verts[i].v, verts[i].v);
			if (animate2) {verts[i].v += fticks*velocity[i];}
		}
		gen_draw_data();
	}
	void render() const { // partially transparent
		if (empty()) return;
		enable_blend();
		psd.draw(BLUR_TEX, 2.0);
		disable_blend();
	}
	void clear() {precip_manager_t<1>::clear(); psd.clear();}
};


rain_manager_t rain_manager;
snow_manager_t snow_manager;
uw_particle_manager_t uw_part_manager;


void draw_local_precipitation(bool no_update) {

	if (!(precip_mode & 1)) return;

	if (temperature <= W_FREEZE_POINT) { // draw snow
		rain_manager.clear();
		//if (world_mode != WMODE_INF_TERRAIN) return; // only drawn snow for tiled terrain?
		if (!no_update) {snow_manager.update();}
		snow_manager.render();
	}
	else { // draw rain
		snow_manager.clear();
		if (!no_update) {rain_manager.update();}
		rain_manager.render();
	}
}


void draw_underwater_particles(float terrain_zmin) {

	if (temperature <= W_FREEZE_POINT) {uw_part_manager.clear(); return;} // frozen, no particles
	//timer_t timer("UW Particles");
	uw_part_manager.update(terrain_zmin);
	uw_part_manager.render();
}

