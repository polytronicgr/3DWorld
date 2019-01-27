// 3D World - Cars for Procedural Cities
// by Frank Gennari
// 11/19/18
#include "city.h"
#include "file_utils.h"
#include "openal_wrap.h"
#include "explosion.h" // for add_blastr()
#include "lightmap.h" // for light_source
#include <cfloat> // for FLT_MAX

float const MIN_CAR_STOP_SEP = 0.25; // in units of car lengths

extern bool tt_fire_button_down;
extern int display_mode, game_mode, animate2;
extern float FAR_CLIP;
extern vector<light_source> dl_sources;
extern city_params_t city_params;


bool city_model_t::read(FILE *fp) { // filename body_material_id fixed_color_id xy_rot dz lod_mult shadow_mat_ids

	assert(fp);
	if (!read_string(fp, fn)) return 0;
	if (!read_int(fp, body_mat_id)) return 0;
	if (!read_int(fp, fixed_color_id)) return 0;
	if (!read_float(fp, xy_rot)) return 0;
	if (!read_float(fp, dz)) return 0;
	if (!read_float(fp, scale)) return 0;
	if (!read_float(fp, lod_mult) || lod_mult <= 0.0) return 0;
	unsigned shadow_mat_id;
	while (read_uint(fp, shadow_mat_id)) {shadow_mat_ids.push_back(shadow_mat_id);}
	return 1;
}


float car_t::get_max_lookahead_dist() const {return (get_length() + city_params.road_width);} // extend one car length + one road width in front
float car_t::get_turn_rot_z(float dist_to_turn) const {return (1.0 - CLIP_TO_01(4.0f*fabs(dist_to_turn)/city_params.road_width));}

void car_t::apply_scale(float scale) {
	if (scale == 1.0) return; // no scale
	float const prev_height(height);
	height *= scale;
	point const pos(get_center());
	bcube.z2() += height - prev_height; // z1 is unchanged
	float const dx(bcube.x2() - pos.x), dy(bcube.y2() - pos.y);
	bcube.x1() = pos.x - scale*dx; bcube.x2() = pos.x + scale*dx;
	bcube.y1() = pos.y - scale*dy; bcube.y2() = pos.y + scale*dy;
}

void car_t::destroy() { // Note: not calling create_explosion(), so no chain reactions
	point const pos(get_center() + get_tiled_terrain_model_xlate());
	float const length(get_length());
	static rand_gen_t rgen;

	for (int n = 0; n < rgen.rand_int(3, 5); ++n) {
		vector3d off(rgen.signed_rand_vector_spherical()*(0.5*length));
		off.z = abs(off.z); // not into the ground
		point const exp_pos(pos + off);
		float const radius(rgen.rand_uniform(1.0, 1.5)*length), time(rgen.rand_uniform(0.3, 0.8));
		add_blastr(exp_pos, (exp_pos - get_camera_pos()), radius, 0.0, time*TICKS_PER_SECOND, CAMERA_ID, YELLOW, RED, ETYPE_ANIM_FIRE, nullptr, 1);
		gen_smoke(exp_pos, 1.0, rgen.rand_uniform(0.4, 0.6));
	} // for n
	gen_delayed_from_player_sound(SOUND_EXPLODE, pos, 1.0);
	park();
	destroyed = 1;
}

float car_t::get_min_sep_dist_to_car(car_t const &c, bool add_one_car_len) const {
	float const avg_len(0.5*(get_length() + c.get_length())); // average length of the two cars
	float const min_speed(max(0.0f, (min(cur_speed, c.cur_speed) - 0.1f*max_speed))); // relative to max speed of 1.0, clamped to 10% at bottom end for stability
	return avg_len*(MIN_CAR_STOP_SEP + 1.11*min_speed + (add_one_car_len ? 1.0 : 0.0)); // 25% to 125% car length, depending on speed (2x on connector roads)
}

string car_t::str() const {
	std::ostringstream oss;
	oss << "Car " << TXT(dim) << TXT(dir) << TXT(cur_city) << TXT(cur_road) << TXT(cur_seg) << TXT(dz) << TXT(max_speed) << TXT(cur_speed)
		<< TXTi(cur_road_type) << TXTi(color_id) << " bcube=" << bcube.str();
	return oss.str();
}

string car_t::label_str() const {
	std::ostringstream oss;
	oss << TXT(dim) << TXTn(dir) << TXT(cur_city) << TXT(cur_road) << TXTn(cur_seg) << TXT(dz) << TXTn(turn_val) << TXT(max_speed) << TXTn(cur_speed)
		<< "wait_time=" << get_wait_time_secs() << "\n" << TXTin(cur_road_type)
		<< TXTn(stopped_at_light) << TXTn(in_isect()) << "cars_in_front=" << count_cars_in_front() << "\n" << TXT(dest_city) << TXTn(dest_isec);
	oss << "car=" << this << " car_in_front=" << car_in_front << endl; // debugging
	return oss.str();
}

void car_t::move(float speed_mult) {
	prev_bcube = bcube;
	if (destroyed || stopped_at_light || is_stopped()) return;
	assert(speed_mult >= 0.0 && cur_speed > 0.0 && cur_speed <= CONN_ROAD_SPEED_MULT*max_speed); // Note: must be valid for connector road => city transitions
	float dist(cur_speed*speed_mult);
	if (dz != 0.0) {dist *= min(1.25, max(0.75, (1.0 - 0.5*dz/get_length())));} // slightly faster down hills, slightly slower up hills
	min_eq(dist, 0.25f*city_params.road_width); // limit to half a car length to prevent cars from crossing an intersection in a single frame
	move_by(dir ? dist : -dist);
	// update waiting state
	float const cur_pos(bcube.d[dim][dir]);
	if (fabs(cur_pos - waiting_pos) > get_length()) {waiting_pos = cur_pos; waiting_start = tfticks;} // update when we move at least a car length
}

void car_t::maybe_accelerate(float mult) {
	if (car_in_front) {
		float const dist_sq(p2p_dist_xy_sq(get_center(), car_in_front->get_center())), length(get_length());

		if (dist_sq > length*length) { // if cars are colliding, let the collision detection system handle it
			float const dmin(get_min_sep_dist_to_car(*car_in_front, 1)); // add_one_car_len=1; space between the two car centers
			if (dist_sq < dmin*dmin) {decelerate(mult); return;} // too close to the car in front - decelerate instead
		}
	}
	accelerate(mult);
}

point car_t::get_front(float dval) const {
	point car_front(get_center());
	car_front[dim] += (dir ? dval : -dval)*get_length(); // half length
	return car_front;
}

bool car_t::front_intersects_car(car_t const &c) const {
	return (c.bcube.contains_pt(get_front(0.25)) || c.bcube.contains_pt(get_front(0.5))); // check front-middle and very front
}

void car_t::honk_horn_if_close() const {
	point const pos(get_center());
	if (dist_less_than((pos + get_tiled_terrain_model_xlate()), get_camera_pos(), 1.0)) {gen_sound(SOUND_HORN, pos);}
}

void car_t::honk_horn_if_close_and_fast() const {
	if (cur_speed > 0.25*max_speed) {honk_horn_if_close();}
}

void car_t::on_alternate_turn_dir(rand_gen_t &rgen) {
	honk_horn_if_close();
	if ((rgen.rand()&3) == 0) {dest_valid = 0;} // 25% chance of choosing a new destination rather than driving in circles; will be in current city
}

void car_t::register_adj_car(car_t &c) {
	if (car_in_front != nullptr && p2p_dist_xy_sq(get_center(), c.get_center()) > p2p_dist_xy_sq(get_center(), car_in_front->get_center())) return; // already found a closer car
	cube_t cube(bcube);
	cube.d[dim][!dir] = cube.d[dim][dir];
	cube.d[dim][dir] += (dir ? 1.0 : -1.0)*get_max_lookahead_dist();
	if (cube.intersects_xy(c.bcube)) {car_in_front = &c;} // projected cube intersects other car
}

unsigned car_t::count_cars_in_front(cube_t const &range) const {
	unsigned num(0);
	car_t const *cur_car(this);

	for (unsigned i = 0; i < 50; ++i) { // limit iterations
		cur_car = cur_car->car_in_front;
		if (!cur_car || (!range.is_all_zeros() && !range.contains_pt_xy(cur_car->get_center()))) break;
		if (cur_car->dim != dim || cur_car->dir == dir) {++num;} // include if not going in opposite direction
	}
	return num;
}

float car_t::get_sum_len_space_for_cars_in_front(cube_t const &range) const {
	float len(0.0);
	car_t const *cur_car(this);

	for (unsigned i = 0; i < 50; ++i) { // limit iterations; avg len = city_params.get_nom_car_size().x (FIXME: 50 not high enough for connector roads)
		if (cur_car->dim != dim || cur_car->dir == dir) {len += cur_car->get_length();} // include if not going in opposite direction
		cur_car = cur_car->car_in_front;
		if (!cur_car || !range.contains_pt_xy(cur_car->get_center())) break;
	}
	return len * (1.0 + MIN_CAR_STOP_SEP); // car length + stopped space (including one extra space for the car behind us)
}

bool car_t::proc_sphere_coll(point &pos, point const &p_last, float radius, vector3d const &xlate, vector3d *cnorm) const {
	return sphere_cube_int_update_pos(pos, radius, (bcube + xlate), p_last, 1, 0, cnorm); // Note: approximate when car is tilted or turning
	//return sphere_sphere_int((bcube.get_cube_center() + xlate), pos, bcube.get_bsphere_radius(), radius, cnorm, pos); // Note: handle cnorm in if using this
}

bool car_t::check_collision(car_t &c, road_gen_base_t const &road_gen) {

	if (c.dim != dim) { // turning in an intersection, etc. (Note: may not be needed, but at least need to return here)
		car_t *to_stop(nullptr);
		if (c.front_intersects_car(*this)) {to_stop = &c;}
		else if (front_intersects_car(c))  {to_stop = this;}
		if (!to_stop) return 0;
		to_stop->decelerate_fast(); // attempt to prevent one car from T-boning the other
		to_stop->bcube = to_stop->prev_bcube;
		to_stop->honk_horn_if_close_and_fast();
		return 1;
	}
	if (dir != c.dir) return 0; // traveling on opposite sides of the road
	float const sep_dist(get_min_sep_dist_to_car(c));
	float const test_dist(0.999*sep_dist); // slightly smaller than separation distance
	cube_t bcube_ext(bcube);
	bcube_ext.d[dim][0] -= test_dist; bcube_ext.d[dim][1] += test_dist; // expand by test_dist distance
	if (!bcube_ext.intersects_xy(c.bcube)) return 0;
	float const front(bcube.d[dim][dir]), c_front(c.bcube.d[dim][dir]);
	bool const move_c((front < c_front) ^ dir); // move the car that's behind
	// Note: we could slow the car in behind, but that won't work for initial placement collisions when speed == 0
	car_t &cmove(move_c ? c : *this); // the car that will be moved
	car_t const &cstay(move_c ? *this : c); // the car that won't be moved
	//cout << "Collision between " << cmove.str() << " and " << cstay.str() << endl;
	if (cstay.is_stopped()) {cmove.decelerate_fast();} else {cmove.decelerate();}
	float const dist(cstay.bcube.d[dim][!dir] - cmove.bcube.d[dim][dir]); // signed distance between the back of the car in front, and the front of the car in back
	point delta(all_zeros);
	delta[dim] += dist + (cmove.dir ? -sep_dist : sep_dist); // force separation between cars
	cube_t const &bcube(road_gen.get_bcube_for_car(cmove));
	if (cstay.max_speed < cmove.max_speed) {cmove.front_car_turn_dir = cstay.turn_dir;} // record the turn dir of this slow car in front of us so we can turn a different way

	if (!bcube.contains_cube_xy(cmove.bcube + delta)) { // moved outside its current road segment bcube
		//if (cmove.bcube == cmove.prev_bcube) {return 1;} // collided, but not safe to move the car (init pos or second collision)
		if (cmove.bcube != cmove.prev_bcube) { // try resetting to last frame's position
			cmove.bcube  = cmove.prev_bcube; // restore prev frame's pos
			//cmove.honk_horn_if_close_and_fast();
			return 1; // done
		}
		else { // keep the car from moving outside its current segment (init collision case)
			if (cmove.dir) {max_eq(delta[dim], min(0.0f, 0.999f*(bcube.d[cmove.dim][0] - cmove.bcube.d[cmove.dim][0])));}
			else           {min_eq(delta[dim], max(0.0f, 0.999f*(bcube.d[cmove.dim][1] - cmove.bcube.d[cmove.dim][1])));}
		}
	}
	cmove.bcube += delta;
	return 1;
}


bool comp_car_road_then_pos::operator()(car_t const &c1, car_t const &c2) const { // sort spatially for collision detection and drawing
	if (c1.cur_city != c2.cur_city) return (c1.cur_city < c2.cur_city);
	if (c1.is_parked() != c2.is_parked()) {return c2.is_parked();} // parked cars last
	if (c1.cur_road != c2.cur_road) return (c1.cur_road < c2.cur_road);

	if (c1.is_parked()) { // sort parked cars back to front relative to camera so that alpha blending works
		return (p2p_dist_sq((c1.bcube.get_cube_center() + xlate), camera_pdu.pos) > p2p_dist_sq((c2.bcube.get_cube_center() + xlate), camera_pdu.pos));
	}
	return (c1.bcube.d[c1.dim][c1.dir] < c2.bcube.d[c2.dim][c2.dir]); // compare front end of car (used for collisions)
}


unsigned car_model_loader_t::num_models() const {return city_params.car_model_files.size();}

city_model_t const &car_model_loader_t::get_model(unsigned id) const {
	assert(id < num_models());
	return city_params.car_model_files[id];
}

bool city_model_loader_t::is_model_valid(unsigned id) {
	assert(id < num_models());
	ensure_models_loaded(); // I guess we have to load the models here to determine if they're valid
	assert(id < models_valid.size());
	return (models_valid[id] != 0);
}

void city_model_loader_t::load_models() {
	models_valid.resize(num_models(), 1); // assume valid

	for (unsigned i = 0; i < num_models(); ++i) {
		string const &fn(get_model(i).fn);
		bool const recalc_normals = 1;

		if (!load_model_file(fn, *this, geom_xform_t(), -1, WHITE, 0, 0.0, recalc_normals, 0, 0, 1)) {
			cerr << "Error: Failed to read model file '" << fn << "'; Skipping this model (will use default low poly model)." << endl;
			push_back(model3d(fn, tmgr)); // add a placeholder dummy model
			models_valid[i] = 0;
		}
	} // for i
}

void city_model_loader_t::draw_model(shader_t &s, vector3d const &pos, cube_t const &obj_bcube, vector3d const &dir, colorRGBA const &color,
	point const &xlate, unsigned model_id, bool is_shadow_pass, bool low_detail)
{
	assert(is_model_valid(model_id));
	assert(size() == num_models()); // must be loaded
	city_model_t const &model_file(get_model(model_id));
	model3d &model(at(model_id));

	if (!is_shadow_pass && model_file.body_mat_id >= 0 && color.A != 0.0) { // use custom color for body material
		material_t &body_mat(model.get_material(model_file.body_mat_id));
		body_mat.ka = body_mat.kd = color;
	}
	model.bind_all_used_tids();
	cube_t const &bcube(model.get_bcube());
	point const orig_camera_pos(camera_pdu.pos);
	camera_pdu.pos += bcube.get_cube_center() - pos - xlate; // required for distance based LOD
	bool const camera_pdu_valid(camera_pdu.valid);
	camera_pdu.valid = 0; // disable VFC, since we're doing custom transforms here
	// Note: in model space, front-back=z, left-right=x, top-bot=y
	float const sz_scale(obj_bcube.get_size().sum() / bcube.get_size().sum());
	fgPushMatrix();
	translate_to(pos + vector3d(0.0, 0.0, model_file.dz*sz_scale));
	if (fabs(dir.y) > 0.001) {rotate_to_plus_x(dir);}
	else if (dir.x < 0.0) {fgRotate(180.0, 0.0, 0.0, 1.0);}
	fgRotate(TO_DEG*asinf(-dir.z), 0.0, 1.0, 0.0);
	if (model_file.xy_rot != 0.0) {fgRotate(model_file.xy_rot, 0.0, 0.0, 1.0);}
	fgRotate(90.0, 1.0, 0.0, 0.0);
	uniform_scale(sz_scale);
	translate_to(-bcube.get_cube_center()); // cancel out model local translate

	if ((low_detail || is_shadow_pass) && !model_file.shadow_mat_ids.empty()) { // low detail pass, normal maps disabled
		if (!is_shadow_pass && use_model3d_bump_maps()) {model3d::bind_default_flat_normal_map();} // still need to set the default here in case the shader is using it
		// TODO: combine shadow materials into a single VBO and draw with one call when is_shadow_pass==1; this is complex and may not yield a significant improvement
		for (auto i = model_file.shadow_mat_ids.begin(); i != model_file.shadow_mat_ids.end(); ++i) {model.render_material(s, *i, is_shadow_pass, 0, 0, 0);}
	}
	else {
		auto const &unbound_mat(model.get_unbound_material());

		for (unsigned sam_pass = 0; sam_pass < (is_shadow_pass ? 2U : 1U); ++sam_pass) {
			model.render_materials(s, is_shadow_pass, 0, 0, (sam_pass == 1), 3, 3, unbound_mat, rotation_t(),
				nullptr, nullptr, is_shadow_pass, model_file.lod_mult, (is_shadow_pass ? 10.0 : 0.0));
		}
	}
	fgPopMatrix();
	camera_pdu.valid = camera_pdu_valid;
	camera_pdu.pos   = orig_camera_pos;
	select_texture(WHITE_TEX); // reset back to default/untextured
}


void car_draw_state_t::occlusion_checker_t::set_camera(pos_dir_up const &pdu) {
	if ((display_mode & 0x08) == 0) {state.building_ids.clear(); return;} // testing
	pos_dir_up near_pdu(pdu);
	near_pdu.far_ = 2.0*city_params.road_spacing; // set far clipping plane to one city block
	get_building_occluders(near_pdu, state);
	//cout << "occluders: " << state.building_ids.size() << endl;
}

bool car_draw_state_t::occlusion_checker_t::is_occluded(cube_t const &c) {
	if (state.building_ids.empty()) return 0;
	float const z(c.z2()); // top edge
	point const corners[4] = {point(c.x1(), c.y1(), z), point(c.x2(), c.y1(), z), point(c.x2(), c.y2(), z), point(c.x1(), c.y2(), z)};
	return check_pts_occluded(corners, 4, state);
}

/*static*/ float car_draw_state_t::get_headlight_dist() {return 3.5*city_params.road_width;} // distance headlights will shine

colorRGBA car_draw_state_t::get_headlight_color(car_t const &car) const {
	return colorRGBA(1.0, 1.0, (1.0 + 0.8*(fract(1000.0*car.max_speed) - 0.5)), 1.0); // slight yellow-blue tinting using max_speed as a hash
}

void car_draw_state_t::pre_draw(vector3d const &xlate_, bool use_dlights_, bool shadow_only) {
	//set_enable_normal_map(use_model3d_bump_maps()); // used only for some car models, and currently doesn't work
	draw_state_t::pre_draw(xlate_, use_dlights_, shadow_only, 1); // always_setup_shader=1 (required for model drawing)
	select_texture(WHITE_TEX);
	if (!shadow_only) {occlusion_checker.set_camera(camera_pdu);}
}

void car_draw_state_t::draw_unshadowed() {
	qbds[0].draw_and_clear();
	if (qbds[2].empty()) return;
	enable_blend();
	select_texture(BLUR_CENT_TEX);
	qbds[2].draw_and_clear();
	select_texture(WHITE_TEX); // reset back to default/untextured
	disable_blend();
}

void car_draw_state_t::add_car_headlights(vector<car_t> const &cars, vector3d const &xlate_, cube_t &lights_bcube) {
	xlate = xlate_; // needed earlier in the flow
	for (auto i = cars.begin(); i != cars.end(); ++i) {add_car_headlights(*i, lights_bcube);}
}

void car_draw_state_t::gen_car_pts(car_t const &car, bool include_top, point pb[8], point pt[8]) const {
	point const center(car.get_center());
	cube_t const &c(car.bcube);
	float const z1(center.z - 0.5*car.height), z2(center.z + 0.5*car.height), zmid(center.z + (include_top ? 0.1 : 0.5)*car.height), length(car.get_length());
	bool const dim(car.dim), dir(car.dir);
	set_cube_pts(c, z1, zmid, dim, dir, pb); // bottom

	if (include_top) {
		cube_t top_part(c);
		top_part.d[dim][0] += (dir ? 0.25 : 0.30)*length; // back
		top_part.d[dim][1] -= (dir ? 0.30 : 0.25)*length; // front
		set_cube_pts(top_part, zmid, z2, dim, dir, pt); // top
	}
	if (car.dz != 0.0) { // rotate all points about dim !d
		float const sine_val((dir ? 1.0 : -1.0)*car.dz/length), cos_val(sqrt(1.0 - sine_val*sine_val));
		rotate_pts(center, sine_val, cos_val, dim, 2, pb);
		if (include_top) {rotate_pts(center, sine_val, cos_val, dim, 2, pt);}
	}
	if (car.rot_z != 0.0) { // turning about the z-axis: rot_z of [0.0, 1.0] maps to angles of [0.0, PI/2=90 degrees]
		float const sine_val(sinf(0.5*PI*car.rot_z)), cos_val(sqrt(1.0 - sine_val*sine_val));
		rotate_pts(center, sine_val, cos_val, 0, 1, pb);
		if (include_top) {rotate_pts(center, sine_val, cos_val, 0, 1, pt);}
	}
}

bool sphere_in_light_cone_approx(pos_dir_up const &pdu, point const &center, float radius) {
	float const dist(p2p_dist(pdu.pos, center)), radius_at_dist(dist*pdu.sterm), rmod(radius_at_dist + radius);
	return pt_line_dist_less_than(center, pdu.pos, (pdu.pos + pdu.dir), rmod);
}

void car_draw_state_t::draw_car(car_t const &car, bool shadow_only, bool is_dlight_shadows) { // Note: all quads
	if (car.destroyed) return;

	if (is_dlight_shadows) { // dynamic spotlight shadow
		point const center(car.get_center());
		if (!dist_less_than(camera_pdu.pos, center, 0.6*camera_pdu.far_)) return; // optimization
		// since we know the dlight is a spotlight with a cone shape rather than a frustum, we can do a tighter visibility test
		if (!sphere_in_light_cone_approx(camera_pdu, center, car.bcube.get_xy_bsphere_radius())) return;
		cube_t bcube(car.bcube);
		bcube.expand_by(0.1*car.height);
		if (bcube.contains_pt(camera_pdu.pos)) return; // don't self-shadow
	}
	if (!check_cube_visible(car.bcube, (shadow_only ? 0.0 : 0.75))) return; // dist_scale=0.75
	point const center(car.get_center());
	begin_tile(center); // enable shadows
	colorRGBA const &color(car.get_color());
	float const dist_val(p2p_dist(camera_pdu.pos, (center + xlate))/get_draw_tile_dist());
	bool const is_truck(car.height > 1.2*city_params.get_nom_car_size().z); // hack - truck has a larger than average size
	bool const draw_top(dist_val < 0.25 && !is_truck), dim(car.dim), dir(car.dir);
	float const sign((dim^dir) ? -1.0 : 1.0);
	point pb[8], pt[8]; // bottom and top sections
	gen_car_pts(car, draw_top, pb, pt);

	if ((shadow_only || dist_val < 0.05) && car_model_loader.is_model_valid(car.model_id)) {
		if (!shadow_only && occlusion_checker.is_occluded(car.bcube + xlate)) return; // only check occlusion for expensive car models
		vector3d const front_n(cross_product((pb[5] - pb[1]), (pb[0] - pb[1])).get_norm()*sign);
		car_model_loader.draw_model(s, center, car.bcube, front_n, color, xlate, car.model_id, shadow_only, (dist_val > 0.035));
	}
	else { // draw simple 1-2 cube model
		quad_batch_draw &qbd(qbds[emit_now]);
		color_wrapper cw(color);
		draw_cube(qbd, cw, center, pb, 1, (dim^dir)); // bottom (skip_bottom=1)
		if (draw_top) {draw_cube(qbd, cw, center, pt, 1, (dim^dir));} // top (skip_bottom=1)
		if (emit_now) {qbds[1].draw_and_clear();} // shadowed (only emit when tile changes?)
	}
	if (dist_val < 0.04 && fabs(car.dz) < 0.01) { // add AO planes when close to the camera and on a level road
		float const length(car.get_length());
		point pao[4];

		for (unsigned i = 0; i < 4; ++i) {
			point &v(pao[i]);
			v = pb[i] - center;
			v[ dim] += 0.1*length*SIGN(v[ dim]); // increase length slightly
			v[!dim] += 0.1*length*SIGN(v[!dim]); // increase width  slightly
			v   += center;
			v.z += 0.02*car.height; // shift up slightly to avoid z-fighting
		}
		/*if (!car.headlights_on()) { // daytime, adjust shadow to match sun pos
		vector3d const sun_dir(0.5*length*(center - get_sun_pos()).get_norm());
		vector3d const offset(sun_dir.x, sun_dir.y, 0.0);
		for (unsigned i = 0; i < 4; ++i) {pao[i] += offset;} // problems: double shadows, non-flat surfaces, buildings, texture coords/back in center, non-rectangular
		}*/
		qbds[2].add_quad_pts(pao, colorRGBA(0, 0, 0, 0.9), plus_z);
	}
	if (dist_val > 0.3)  return; // to far - no lights to draw
	if (shadow_only || car.is_parked()) return; // no lights when parked, or in shadow pass
	vector3d const front_n(cross_product((pb[5] - pb[1]), (pb[0] - pb[1])).get_norm()*sign);
	unsigned const lr_xor(((camera_pdu.pos[!dim] - xlate[!dim]) - center[!dim]) < 0.0);
	bool const brake_lights_on(car.is_almost_stopped() || car.stopped_at_light), headlights_on(car.headlights_on());

	if (headlights_on && dist_val < 0.3) { // night time headlights
		colorRGBA const hl_color(get_headlight_color(car));

		for (unsigned d = 0; d < 2; ++d) { // L, R
			unsigned const lr(d ^ lr_xor ^ 1);
			point const pos((lr ? 0.2 : 0.8)*(0.2*pb[0] + 0.8*pb[4]) + (lr ? 0.8 : 0.2)*(0.2*pb[1] + 0.8*pb[5]));
			add_light_flare(pos, front_n, hl_color, 2.0, 0.65*car.height); // pb 0,1,4,5
		}
	}
	if ((brake_lights_on || headlights_on) && dist_val < 0.2) { // brake lights
		for (unsigned d = 0; d < 2; ++d) { // L, R
			unsigned const lr(d ^ lr_xor);
			point const pos((lr ? 0.2 : 0.8)*(0.2*pb[2] + 0.8*pb[6]) + (lr ? 0.8 : 0.2)*(0.2*pb[3] + 0.8*pb[7]));
			add_light_flare(pos, -front_n, RED, (brake_lights_on ? 1.0 : 0.5), 0.5*car.height); // pb 2,3,6,7
		}
	}
	if (car.turn_dir != TURN_NONE && car.cur_city != CONN_CITY_IX && dist_val < 0.1) { // turn signals (not on connector road bends)
		float const ts_period = 1.5; // in seconds
		double const time(fract((tfticks + 1000.0*car.max_speed)/(ts_period*TICKS_PER_SECOND))); // use car max_speed as seed to offset time base

		if (time > 0.5) { // flash on and off
			bool const tdir((car.turn_dir == TURN_LEFT) ^ dim ^ dir); // R=1,2,5,6 or L=0,3,4,7
			vector3d const side_n(cross_product((pb[6] - pb[2]), (pb[1] - pb[2])).get_norm()*sign*(tdir ? 1.0 : -1.0));

			for (unsigned d = 0; d < 2; ++d) { // B, F
				point const pos(0.3*pb[tdir ? (d ? 1 : 2) : (d ? 0 : 3)] + 0.7*pb[tdir ? (d ? 5 : 6) : (d ? 4 : 7)]);
				add_light_flare(pos, (side_n + (d ? 1.0 : -1.0)*front_n).get_norm(), colorRGBA(1.0, 0.75, 0.0, 1.0), 1.5, 0.3*car.height); // normal points out 45 degrees
			}
		}
	}
}

void car_draw_state_t::add_car_headlights(car_t const &car, cube_t &lights_bcube) {
	if (!car.headlights_on()) return;
	float const headlight_dist(get_headlight_dist());
	cube_t bcube(car.bcube);
	bcube.expand_by(headlight_dist);
	if (!lights_bcube.contains_cube_xy(bcube))   return; // not contained within the light volume
	if (!camera_pdu.cube_visible(bcube + xlate)) return; // VFC
	float const sign((car.dim^car.dir) ? -1.0 : 1.0);
	point pb[8], pt[8]; // bottom and top sections
	gen_car_pts(car, 0, pb, pt); // draw_top=0
	vector3d const front_n(cross_product((pb[5] - pb[1]), (pb[0] - pb[1])).get_norm()*sign);
	vector3d const dir((0.5*front_n - 0.5*plus_z).get_norm()); // point slightly down
	colorRGBA const color(get_headlight_color(car));
	float const beamwidth = 0.08;
	min_eq(lights_bcube.z1(), bcube.z1());
	max_eq(lights_bcube.z2(), bcube.z2());

	// TODO: merge headlights into a single spotlight for distant cars? share shadow maps between headlights?
	for (unsigned d = 0; d < 2; ++d) { // L, R
		point const pos((d ? 0.2 : 0.8)*(0.2*pb[0] + 0.8*pb[4]) + (d ? 0.8 : 0.2)*(0.2*pb[1] + 0.8*pb[5]));
		dl_sources.push_back(light_source(headlight_dist, pos, pos, color, 1, dir, beamwidth));
	}
}


void car_manager_t::remove_destroyed_cars() {
	remove_destroyed(cars);
	car_destroyed = 0;
}

void car_manager_t::init_cars(unsigned num) {
	if (num == 0) return;
	timer_t timer("Init Cars");
	cars.reserve(num);
	for (unsigned n = 0; n < num; ++n) {add_car();}
	cout << "Dynamic Cars: " << cars.size() << endl;
}

void car_manager_t::finalize_cars() {
	if (empty()) return;
	unsigned const num_models(car_model_loader.num_models());

	for (auto i = cars.begin(); i != cars.end(); ++i) {
		int fixed_color(-1);

		if (num_models > 0) {
			if (FORCE_MODEL_ID >= 0) {i->model_id = FORCE_MODEL_ID;}
			else {i->model_id = ((num_models > 1) ? (rgen.rand() % num_models) : 0);}
			city_model_t const &model(car_model_loader.get_model(i->model_id));
			fixed_color = model.fixed_color_id;
			i->apply_scale(model.scale);
		}
		i->color_id = ((fixed_color >= 0) ? fixed_color : (rgen.rand() % NUM_CAR_COLORS));
		assert(i->is_valid());
	} // for i
	cout << "Total Cars: " << cars.size() << endl;
}

bool car_manager_t::proc_sphere_coll(point &pos, point const &p_last, float radius, vector3d *cnorm) const {
	vector3d const xlate(get_camera_coord_space_xlate());
	float const dist(p2p_dist(pos, p_last));

	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		cube_t const city_bcube(get_cb_bcube(*cb) + xlate);
		if (pos.z - radius > city_bcube.z2() + city_params.get_max_car_size().z) continue; // above the cars
		if (!sphere_cube_intersect_xy(pos, (radius + dist), city_bcube)) continue;
		cube_t sphere_bc; sphere_bc.set_from_sphere((pos - xlate), radius);
		unsigned start(0), end(0);
		get_car_ix_range_for_cube(cb, sphere_bc, start, end);

		for (unsigned c = start; c != end; ++c) {
			if (cars[c].proc_sphere_coll(pos, p_last, radius, xlate, cnorm)) return 1;
		}
	} // for cb
	return 0;
}

void car_manager_t::destroy_cars_in_radius(point const &pos_in, float radius) {
	vector3d const xlate(get_camera_coord_space_xlate());
	point const pos(pos_in - xlate);
	bool const is_pt(radius == 0.0);

	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		cube_t const city_bcube(get_cb_bcube(*cb));
		if (pos.z - radius > city_bcube.z2() + city_params.get_max_car_size().z) continue; // above the cars
		if (is_pt ? !city_bcube.contains_pt_xy(pos) : !sphere_cube_intersect_xy(pos, radius, city_bcube)) continue;
		unsigned const start(cb->start), end((cb+1)->start); // Note: shouldnt be called frequently enough to need road/parking lot acceleration
		assert(end <= cars.size() && start <= end);

		for (unsigned c = start; c != end; ++c) {
			car_t &car(cars[c]);

			if (is_pt ? car.bcube.contains_pt(pos) : dist_less_than(car.get_center(), pos, radius)) { // destroy if within the sphere
				car.destroy();
				car_destroyed = 1;
				// invalidate tile shadow map for destroyed parked cars
				if (city_params.car_shadows && car.is_parked()) {invalidate_tile_smap_at_pt((car.get_center() + xlate), 0.5*car.get_length());} // radius = length/2
			}
		} // for c
	} // for cb
}

bool car_manager_t::get_color_at_xy(point const &pos, colorRGBA &color, int int_ret) const { // Note: pos in local TT space
	if (cars.empty()) return 0;
	if (int_ret != INT_ROAD && int_ret != INT_PARKING) return 0; // not a road or a parking lot - no car intersections

	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		if (!get_cb_bcube(*cb).contains_pt_xy(pos)) continue; // skip
		unsigned start(cb->start), end((cb+1)->start);
		assert(end <= cars.size());
		if      (int_ret == INT_ROAD)    {end   = cb->first_parked;} // moving cars only (beginning of range)
		else if (int_ret == INT_PARKING) {start = cb->first_parked;} // parked cars only (end of range)
		assert(start <= end);

		for (unsigned c = start; c != end; ++c) { // Note: slow, could use road as accel structure
			if (cars[c].bcube.contains_pt_xy(pos)) {color = cars[c].get_color(); return 1;}
		}
	} // for cb
	return 0;
}

car_t const *car_manager_t::get_car_at(point const &p1, point const &p2) const { // Note: p1/p2 in local TT space
	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		if (!get_cb_bcube(*cb).line_intersects(p1, p2)) continue; // skip
		unsigned start(cb->start), end((cb+1)->start);
		assert(start <= end && end <= cars.size());

		for (unsigned c = start; c != end; ++c) { // Note: includes parked cars
			if (cars[c].bcube.line_intersects(p1, p2)) {return &cars[c];}
		}
	} // for cb
	return nullptr; // no car found
}

bool car_manager_t::line_intersect_cars(point const &p1, point const &p2, float &t) const { // Note: p1/p2 in local TT space
	bool ret(0);

	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		if (!get_cb_bcube(*cb).line_intersects(p1, p2)) continue; // skip
		unsigned start(cb->start), end((cb+1)->start);
		assert(start <= end && end <= cars.size());

		for (unsigned c = start; c != end; ++c) { // Note: includes parked cars
			ret |= check_line_clip_update_t(p1, p2, t, cars[c].bcube);
		}
	} // for cb
	return ret;
}

int car_manager_t::find_next_car_after_turn(car_t &car) {
	road_isec_t const &isec(get_car_isec(car));
	if (car.turn_dir == TURN_NONE && !isec.is_global_conn_int()) return -1; // car not turning, and not on connector road isec: should be handled by sorted car_in_front logic
	unsigned const dest_orient(isec.get_dest_orient_for_car_in_isec(car, 0)); // Note: may be before, during, or after turning
	int road_ix(isec.rix_xy[dest_orient]), seg_ix(isec.conn_ix[dest_orient]);
	unsigned city_ix(car.cur_city);
	//cout << TXT(car.get_orient()) << TXT(dest_orient) << TXT(city_ix) << TXT(road_ix) << TXT(seg_ix) << endl;
	assert((road_ix < 0) == (seg_ix < 0));

	if (road_ix < 0) { // goes to connector road
		city_ix = CONN_CITY_IX;
		road_ix = decode_neg_ix(road_ix);
		seg_ix  = decode_neg_ix(seg_ix );
	}
	point const car_center(car.get_center());
	float dmin(car.get_max_lookahead_dist()), dmin_sq(dmin*dmin);
	// include normal sorted order car; this is needed when going straight through connector road 4-way intersections where cur_road changes within the intersection
	if (car.car_in_front && car.car_in_front->get_orient() != dest_orient) {car.car_in_front = 0;} // not the correct car (turning a different way)
	if (car.turn_dir == TURN_NONE && car.car_in_front) {min_eq(dmin_sq, p2p_dist_sq(car_center, car.car_in_front->get_center()));}
	int ret_car_ix(-1);

	for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
		if (cb->cur_city != city_ix) continue; // incorrect city - skip
		unsigned const start(cb->start), end(cb->first_parked);
		assert(end <= cars.size() && start <= end);
		auto range_end(cars.begin()+end);
		car_t ref_car; ref_car.cur_road = road_ix;
		auto it(std::lower_bound(cars.begin()+start, range_end, ref_car, comp_car_road())); // binary search acceleration
		float prev_dist_sq(FLT_MAX);

		for (; it != range_end; ++it) {
			if (&(*it) == &car) continue; // skip self
			assert(it->cur_city == city_ix); // must be same city
			if (it->cur_road != road_ix) break; // different road, done

			if (it->cur_road_type == TYPE_RSEG) { // road segment
				if (it->cur_seg != seg_ix) continue; // on a different segment, skip
			}
			else if (it->cur_road_type != car.cur_road_type || it->cur_seg != car.cur_seg) continue; // in a different intersection
			if (it->get_orient() != dest_orient) continue; // wrong orient
			float const dist_sq(p2p_dist_sq(car_center, it->get_center()));
			if (p2p_dist_sq(car_center, it->get_front()) < dist_sq) continue; // front is closer than back - this car is not in front of us (waiting on other side of isect?)
																			  //cout << TXT(dmin_sq) << TXT(dist_sq) << (dist_sq < dmin_sq) << endl;

			if (dist_sq < dmin_sq) { // new closest car
				if (&(*it) != car.car_in_front) {ret_car_ix = (it - cars.begin());} // record index if set to a new value
				car.car_in_front = &(*it);
				dmin_sq = dist_sq;
			}
			else if (dist_sq > prev_dist_sq) break; // we're moving too far away from the car
			prev_dist_sq = dist_sq;
		} // for it
	} // for cb
	return ret_car_ix;
}

void car_manager_t::next_frame(float car_speed) {
	if (cars.empty() || !animate2) return;
	//timer_t timer("Update Cars"); // 4K cars = 0.7ms / 1.2ms with destinations + navigation
	if (car_destroyed) {remove_destroyed_cars();} // at least one car was destroyed in the previous frame - remove it/them
	sort(cars.begin(), cars.end(), comp_car_road_then_pos(dstate.xlate)); // sort by city/road/position for intersection tests and tile shadow map binds
	entering_city.clear();
	car_blocks.clear();
	float const speed(0.001*car_speed*fticks);
	bool saw_parked(0);
	//unsigned num_on_conn_road(0);

	for (auto i = cars.begin(); i != cars.end(); ++i) { // move cars
		unsigned const cix(i - cars.begin());
		i->car_in_front = nullptr; // reset for this frame

		if (car_blocks.empty() || i->cur_city != car_blocks.back().cur_city) {
			if (!saw_parked && !car_blocks.empty()) {car_blocks.back().first_parked = cix;}
			saw_parked = 0;
			car_blocks.emplace_back(cix, i->cur_city);
		}
		if (i->is_parked()) {
			if (!saw_parked) {car_blocks.back().first_parked = cix; saw_parked = 1;}
			continue; // no update for parked cars
		}
		i->move(speed);
		if (i->entering_city) {entering_city.push_back(cix);} // record for use in collision detection
		if (!i->stopped_at_light && i->is_almost_stopped() && i->in_isect()) {get_car_isec(*i).stoplight.mark_blocked(i->dim, i->dir);} // blocking intersection
		register_car_at_city(*i);
	} // for i
	if (!saw_parked && !car_blocks.empty()) {car_blocks.back().first_parked = cars.size();}
	car_blocks.emplace_back(cars.size(), 0); // add terminator

	for (auto i = cars.begin(); i != cars.end(); ++i) { // collision detection
		if (i->is_parked()) continue; // no collisions for parked cars
		bool const on_conn_road(i->cur_city == CONN_CITY_IX);
		float const length(i->get_length()), max_check_dist(max(3.0f*length, (length + i->get_max_lookahead_dist()))); // max of collision dist and car-in-front dist

		for (auto j = i+1; j != cars.end(); ++j) { // check for collisions with cars on the same road (can't test seg because they can be on diff segs but still collide)
			if (i->cur_city != j->cur_city || i->cur_road != j->cur_road) break; // different cities or roads
			if (!on_conn_road && i->cur_road_type == j->cur_road_type && abs((int)i->cur_seg - (int)j->cur_seg) > (on_conn_road ? 1 : 0)) break; // diff road segs or diff isects
			check_collision(*i, *j);
			i->register_adj_car(*j);
			j->register_adj_car(*i);
			if (!dist_xy_less_than(i->get_center(), j->get_center(), max_check_dist)) break;
		}
		if (on_conn_road) { // on connector road, check before entering intersection to a city
			for (auto ix = entering_city.begin(); ix != entering_city.end(); ++ix) {
				if (*ix != (i - cars.begin())) {check_collision(*i, cars[*ix]);}
			}
			//++num_on_conn_road;
		}
		if (i->in_isect()) {
			int const next_car(find_next_car_after_turn(*i)); // Note: calculates in i->car_in_front
			if (next_car >= 0) {check_collision(*i, cars[next_car]);} // make sure we collide with the correct car
		}
	} // for i
	update_cars(); // run update logic
	//cout << TXT(cars.size()) << TXT(entering_city.size()) << TXT(in_isects.size()) << TXT(num_on_conn_road) << endl; // TESTING
}

void car_manager_t::draw(int trans_op_mask, vector3d const &xlate, bool use_dlights, bool shadow_only, bool is_dlight_shadows) {
	if (cars.empty()) return;

	if (trans_op_mask & 1) { // opaque pass, should be first
		if (is_dlight_shadows && !city_params.car_shadows) return;
		bool const only_parked(shadow_only && !is_dlight_shadows); // sun/moon shadows are precomputed and cached, so only include static objects such as parked cars
		//timer_t timer(string("Draw Cars") + (shadow_only ? " Shadow" : "")); // 10K cars = 1.5ms / 2K cars = 0.33ms
		dstate.xlate = xlate;
		fgPushMatrix();
		translate_to(xlate);
		dstate.pre_draw(xlate, use_dlights, shadow_only);

		for (auto cb = car_blocks.begin(); cb+1 < car_blocks.end(); ++cb) {
			if (!camera_pdu.cube_visible(get_cb_bcube(*cb) + xlate)) continue; // city not visible - skip
			unsigned const end((cb+1)->start);
			assert(end <= cars.size());

			for (unsigned c = cb->start; c != end; ++c) {
				if (only_parked && !cars[c].is_parked()) continue; // skip non-parked cars
				dstate.draw_car(cars[c], shadow_only, is_dlight_shadows);
			}
		} // for cb
		dstate.post_draw();
		fgPopMatrix();

		if (tt_fire_button_down && !game_mode) {
			point const p1(get_camera_pos() - xlate), p2(p1 + cview_dir*FAR_CLIP);
			car_t const *car(get_car_at(p1, p2));
			if (car != nullptr) {dstate.set_label_text(car->label_str(), (car->get_center() + xlate));} // car found
		}
	}
	if ((trans_op_mask & 2) && !shadow_only) {dstate.draw_and_clear_light_flares();} // transparent pass; must be done last for alpha blending, and no translate
	dstate.show_label_text();
}

