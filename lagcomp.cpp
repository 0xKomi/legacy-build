#include "includes.h"

LagCompensation g_lagcomp{};;

bool LagCompensation::StartPrediction(AimPlayer* data) {
	// we have no data to work with.
	// this should never happen if we call this
	if ( data->m_records.size( ) <= 0 )
		return false;

	// meme.
	if ( data->m_player->dormant( ) )
		return false;

	// compute the true amount of updated records
	// since the last time the player entered pvs.
	size_t size{};

	// iterate records.
	for ( const auto& it : data->m_records ) {
		if ( it->dormant( ) )
			break;

		// increment total amount of data.
		++size;
	}

	// get first record.
	LagRecord* record = data->m_records[ 0 ].get( );

	// reset all prediction related variables.
	// this has been a recurring problem in all my hacks lmfao.
	// causes the prediction to stack on eachother.
	record->predict( );

	// check if lc broken.
	if ( size > 1 && ( ( record->m_origin - data->m_records[ 1 ]->m_origin ).length_sqr( ) > 4096.f
		|| size > 2 && ( data->m_records[ 1 ]->m_origin - data->m_records[ 2 ]->m_origin ).length_sqr( ) > 4096.f ) )
		record->m_broke_lc = true;

	// we are not breaking lagcomp at this point.
	// return false so it can aim at all the records it once
	// since server-sided lagcomp is still active and we can abuse that.
	if ( !record->m_broke_lc )
		return false;

	int simulation = game::TIME_TO_TICKS( record->m_sim_time );

	// this is too much lag to fix.
	if ( std::abs( core.m_arrival_tick - simulation ) >= 128 )
		return true;

	// compute the amount of lag that we will predict for, if we have one set of data, use that.
	// if we have more data available, use the prevoius lag delta to counter weird fakelags that switch between 14 and 2.
	int lag = ( size <= 2 ) ? game::TIME_TO_TICKS( record->m_sim_time - data->m_records[ 1 ]->m_sim_time )
		: game::TIME_TO_TICKS( data->m_records[ 1 ]->m_sim_time - data->m_records[ 2 ]->m_sim_time );

	// clamp this just to be sure.
	math::clamp( lag, 1, 15 );

	// get the delta in ticks between the last server net update
	// and the net update on which we created this record.
	int updatedelta = core.m_server_tick - record->m_tick;

	// if the lag delta that is remaining is less than the current netlag
	// that means that we can shoot now and when our shot will get processed
	// the origin will still be valid, therefore we do not have to predict.
	if ( core.m_latency_ticks <= lag - updatedelta )
		return true;

	// the next update will come in, wait for it.
	int next = record->m_tick + 1;
	if ( next + lag >= core.m_arrival_tick )
		return true;

	return true;
}

void LagCompensation::PlayerMove(LagRecord* record) {
	vec3_t                start, end, normal;
	CGameTrace            trace;
	CTraceFilterWorldOnly filter;

	// define trace start.
	start = record->m_pred_origin;

	// move trace end one tick into the future using predicted velocity.
	end = start + (record->m_pred_velocity * g_csgo.m_globals->m_interval);

	// trace.
	g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);

	// we hit shit
	// we need to fix hit.
	if (trace.m_fraction != 1.f) {

		// fix sliding on planes.
		for (int i{}; i < 2; ++i) {
			record->m_pred_velocity -= trace.m_plane.m_normal * record->m_pred_velocity.dot(trace.m_plane.m_normal);

			float adjust = record->m_pred_velocity.dot(trace.m_plane.m_normal);
			if (adjust < 0.f)
				record->m_pred_velocity -= (trace.m_plane.m_normal * adjust);

			start = trace.m_endpos;
			end = start + (record->m_pred_velocity * (g_csgo.m_globals->m_interval * (1.f - trace.m_fraction)));

			g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);
			if (trace.m_fraction == 1.f)
				break;
		}
	}

	// set new final origin.
	start = end = record->m_pred_origin = trace.m_endpos;

	// move endpos 2 units down.
	// this way we can check if we are in/on the ground.
	end.z -= 2.f;

	// trace.
	g_csgo.m_engine_trace->TraceRay(Ray(start, end, record->m_mins, record->m_maxs), CONTENTS_SOLID, &filter, &trace);

	// strip onground flag.
	record->m_pred_flags &= ~FL_ONGROUND;

	// add back onground flag if we are onground.
	if (trace.m_fraction != 1.f && trace.m_plane.m_normal.z > 0.7f)
		record->m_pred_flags |= FL_ONGROUND;
}

void LagCompensation::AirAccelerate(LagRecord* record, ang_t angle, float fmove, float smove) {
	vec3_t fwd, right, wishvel, wishdir;
	float  maxspeed, wishspd, wishspeed, currentspeed, addspeed, accelspeed;

	// determine movement angles.
	math::AngleVectors(angle, &fwd, &right);

	// zero out z components of movement vectors.
	fwd.z = 0.f;
	right.z = 0.f;

	// normalize remainder of vectors.
	fwd.normalize();
	right.normalize();

	// determine x and y parts of velocity.
	for (int i{}; i < 2; ++i)
		wishvel[i] = (fwd[i] * fmove) + (right[i] * smove);

	// zero out z part of velocity.
	wishvel.z = 0.f;

	// determine maginitude of speed of move.
	wishdir = wishvel;
	wishspeed = wishdir.normalize();

	// get maxspeed.
	// TODO; maybe global this or whatever its 260 anyway always.
	maxspeed = record->m_player->m_flMaxspeed();

	// clamp to server defined max speed.
	if (wishspeed != 0.f && wishspeed > maxspeed)
		wishspeed = maxspeed;

	// make copy to preserve original variable.
	wishspd = wishspeed;

	// cap speed.
	if (wishspd > 30.f)
		wishspd = 30.f;

	// determine veer amount.
	currentspeed = record->m_pred_velocity.dot(wishdir);

	// see how much to add.
	addspeed = wishspd - currentspeed;

	// if not adding any, done.
	if (addspeed <= 0.f)
		return;

	// Determine acceleration speed after acceleration
	accelspeed = g_csgo.sv_airaccelerate->GetFloat() * wishspeed * g_csgo.m_globals->m_interval;

	// cap it.
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	// add accel.
	record->m_pred_velocity += (wishdir * accelspeed);
}

// testing new shits.
void LagCompensation::PredictAnimations(CCSGOPlayerAnimState* state, LagRecord* record) {
	struct AnimBackup_t {
		float  curtime;
		float  frametime;
		int    flags;
		int    eflags;
		vec3_t velocity;
	};

	// get player ptr.
	Player* player = record->m_player;

	// backup data.
	AnimBackup_t backup;
	backup.curtime = g_csgo.m_globals->m_curtime;
	backup.frametime = g_csgo.m_globals->m_frametime;
	backup.flags = player->m_fFlags( );
	backup.eflags = player->m_iEFlags( );
	backup.velocity = player->m_vecAbsVelocity( );

	// set globals appropriately for animation.
	g_csgo.m_globals->m_curtime = record->m_pred_time;
	g_csgo.m_globals->m_frametime = g_csgo.m_globals->m_interval;

	// EFL_DIRTY_ABSVELOCITY
	// skip call to C_BaseEntity::CalcAbsoluteVelocity
	player->m_iEFlags( ) &= ~0x1000;

	// set predicted flags and velocity.
	player->m_fFlags( ) = record->m_pred_flags;
	player->m_vecAbsVelocity( ) = record->m_pred_velocity;

	// enable re-animation in the same frame if animated already.
	if ( state->m_frame >= g_csgo.m_globals->m_frame )
		state->m_frame = g_csgo.m_globals->m_frame - 1;

	bool fake = g_menu.main.aimbot.correct.get( );

	// rerun the resolver since we edited the origin.
	if ( fake )
		g_resolver.ResolveAngles( player, record );

	// update animations.
	game::UpdateAnimationState( state, record->m_eye_angles );

	// rerun the pose correction cuz we are re-setupping them.
	if ( fake )
		g_resolver.ResolvePoses( player, record );

	// get new rotation poses and layers.
	player->GetPoseParameters( record->m_poses );
	player->GetAnimLayers( record->m_layers );
	record->m_abs_ang = player->GetAbsAngles( );

	// restore globals.
	g_csgo.m_globals->m_curtime = backup.curtime;
	g_csgo.m_globals->m_frametime = backup.frametime;

	// restore player data.
	player->m_fFlags( ) = backup.flags;
	player->m_iEFlags( ) = backup.eflags;
	player->m_vecAbsVelocity( ) = backup.velocity;
}