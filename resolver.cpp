#include "includes.h"
Resolver g_resolver{};;

LagRecord* Resolver::FindIdealRecord( AimPlayer* data ) {
	LagRecord* first_valid, * current;

	if ( data->m_records.empty( ) )
		return nullptr;

	first_valid = nullptr;

	// iterate records.
	for ( const auto& it : data->m_records ) {
		if ( it->dormant( ) || it->immune( ) || !it->valid( ) )
			continue;

		// get current record.
		current = it.get( );

		// first record that was valid, store it for later.
		if ( !first_valid )
			first_valid = current;

		// try to find a record with a shot, lby update, walking or no anti-aim.
		if ( it->m_shot || it->m_mode == Modes::RESOLVE_BODY || it->m_mode == Modes::RESOLVE_WALK || it->m_mode == Modes::RESOLVE_NONE )
			return current;
	}

	// none found above, return the first valid record if possible.
	return ( first_valid ) ? first_valid : nullptr;
}

LagRecord* Resolver::FindLastRecord( AimPlayer* data ) {
	LagRecord* current;

	if ( data->m_records.empty( ) )
		return nullptr;

	// iterate records in reverse.
	for ( auto it = data->m_records.crbegin( ); it != data->m_records.crend( ); ++it ) {
		current = it->get( );

		// if this record is valid.
		// we are done since we iterated in reverse.
		if ( current->valid( ) && !current->immune( ) && !current->dormant( ) )
			return current;
	}

	return nullptr;
}

void Resolver::OnBodyUpdate( Player* player, float value ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];

	// set data.
	data->m_old_body = data->m_body;
	data->m_body = value;
}

float Resolver::GetAwayAngle( LagRecord* record ) {
	float  delta{ std::numeric_limits< float >::max( ) };
	vec3_t pos;
	ang_t  away;

	if ( core.m_net_pos.empty( ) ) {
		math::VectorAngles( core.m_local->m_vecOrigin( ) - record->m_pred_origin, away );
		return away.y;
	}

	float owd = ( core.m_latency / 2.f );
	float target = record->m_pred_time;

	// iterate all.
	for ( const auto& net : core.m_net_pos ) {
		float dt = std::abs( target - net.m_time );

		// the best origin.
		if ( dt < delta ) {
			delta = dt;
			pos = net.m_pos;
		}
	}

	math::VectorAngles( pos - record->m_pred_origin, away );
	return away.y;
}

void Resolver::MatchShot( AimPlayer* data, LagRecord* record ) {
	// do not attempt to do this in nospread mode.
	if ( g_menu.main.config.mode.get( ) == 1 )
		return;

	float shoot_time = -1.f;

	Weapon* weapon = data->m_player->GetActiveWeapon( );
	if ( weapon ) {
		shoot_time = weapon->m_fLastShotTime( ) + g_csgo.m_globals->m_interval;
	}

	float delta = game::TIME_TO_TICKS( shoot_time ) - game::TIME_TO_TICKS( record->m_sim_time );

	//new
	if ( delta == 1 )
	{
		record->m_shot = true;
	}
	else if ( delta == 0 ) {
		record->m_shot = false;

		if ( data->m_records.size( ) >= 2 )
		{
			LagRecord* previous = data->m_records[ 1 ].get( );
			if ( previous && !previous->dormant( ) )
				record->m_eye_angles.x = previous->m_eye_angles.x;
		}
	}
}

void Resolver::AntiFreestand( LagRecord* record ) {
	// constants
	constexpr float STEP{ 4.f };
	constexpr float RANGE{ 32.f };

	// best target.
	vec3_t enemypos = record->m_player->GetShootPosition( );
	float away = GetAwayAngle( record );

	// construct vector of angles to test.
	std::vector< AdaptiveAngle > angles{ };
	angles.emplace_back( away - 180.f );
	angles.emplace_back( away + 90.f );
	angles.emplace_back( away - 90.f );

	// start the trace at the your shoot pos.
	vec3_t start = core.m_local->GetShootPosition( );

	// see if we got any valid result.
	// if this is false the path was not obstructed with anything.
	bool valid{ false };

	// iterate vector of angles.
	for ( auto it = angles.begin( ); it != angles.end( ); ++it ) {

		// compute the 'rough' estimation of where our head will be.
		vec3_t end{ enemypos.x + std::cos( math::deg_to_rad( it->m_yaw ) ) * RANGE,
			enemypos.y + std::sin( math::deg_to_rad( it->m_yaw ) ) * RANGE,
			enemypos.z };

		// draw a line for debugging purposes.
		// g_csgo.m_debug_overlay->AddLineOverlay( start, end, 255, 0, 0, true, 0.1f );

		// compute the direction.
		vec3_t dir = end - start;
		float len = dir.normalize( );

		// should never happen.
		if ( len <= 0.f )
			continue;

		// step thru the total distance, 4 units per step.
		for ( float i{ 0.f }; i < len; i += STEP ) {
			// get the current step position.
			vec3_t point = start + ( dir * i );

			// get the contents at this point.
			int contents = g_csgo.m_engine_trace->GetPointContents( point, MASK_SHOT_HULL );

			// contains nothing that can stop a bullet.
			if ( !( contents & MASK_SHOT_HULL ) )
				continue;

			float mult = 1.f;

			// over 50% of the total length, prioritize this shit.
			if ( i > ( len * 0.5f ) )
				mult = 1.25f;

			// over 90% of the total length, prioritize this shit.
			if ( i > ( len * 0.75f ) )
				mult = 1.25f;

			// over 90% of the total length, prioritize this shit.
			if ( i > ( len * 0.9f ) )
				mult = 2.f;

			// append 'penetrated distance'.
			it->m_dist += ( STEP * mult );

			// mark that we found anything.
			valid = true;
		}
	}

	if ( !valid ) {
		return;
	}

	// put the most distance at the front of the container.
	std::sort( angles.begin( ), angles.end( ),
		[ ]( const AdaptiveAngle& a, const AdaptiveAngle& b ) {
			return a.m_dist > b.m_dist;
		} );

	// the best angle should be at the front now.
	AdaptiveAngle* best = &angles.front( );

	record->m_eye_angles.y = best->m_yaw;
}

void Resolver::SetMode( LagRecord* record ) {
	// the resolver has 3 modes to chose from.
	// these modes will vary more under the hood depending on what data we have about the player
	// and what kind of hack vs. hack we are playing (mm/nospread).

	float speed = record->m_velocity.length_2d( );

	// if on ground, moving, and not fakewalking.
	if ( ( record->m_flags & FL_ONGROUND ) && speed > 0.2f && !record->m_fake_walk )
		record->m_mode = Modes::RESOLVE_WALK;

	// if on ground, not moving or fakewalking.
	else if ( ( record->m_flags & FL_ONGROUND ) && ( speed <= 0.1f || record->m_fake_walk ) && !g_input.GetKeyState( g_menu.main.aimbot.override.get( ) ) )
		record->m_mode = Modes::RESOLVE_LASTMOVE;

	// if not on ground.
	else if ( !( record->m_flags & FL_ONGROUND ) )
		record->m_mode = Modes::RESOLVE_AIR;
}

void Resolver::ResolveAngles( Player* player, LagRecord* record ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];
	LagRecord* move = &data->m_walk_record;

	float delta = record->m_anim_time - move->m_anim_time;

	C_AnimationLayer* curr = &record->m_layers[ 3 ];
	const int activity = data->m_player->GetSequenceActivity( curr->m_sequence );

	// mark this record if it contains a shot.
	MatchShot( data, record );

	// next up mark this record with a resolver mode that will be used.
	SetMode( record );

	// if we are in nospread mode, force all players pitches to down.
	// TODO; we should check thei actual pitch and up too, since those are the other 2 possible angles.
	// this should be somehow combined into some iteration that matches with the air angle iteration.
	if ( g_menu.main.config.mode.get( ) == 1 )
		record->m_eye_angles.x = 90.f;

	// we arrived here we can do the acutal resolve.
	if ( record->m_mode == Modes::RESOLVE_WALK )
		ResolveWalk( data, record );

	else if ( record->m_mode == Modes::RESOLVE_LASTMOVE || record->m_mode == Modes::RESOLVE_UNKNOWM && !( g_input.GetKeyState( g_menu.main.aimbot.override.get( ) ) ) )
		LastMoveLby( record, data, player );

	else if ( record->m_mode == Modes::RESOLVE_AIR )
		ResolveAir( data, record, player );

	else if ( activity == 979 && curr->m_weight == 0 && delta > .22f )
		AntiFreestand( record );

	// normalize the eye angles, doesn't really matter but its clean.
	math::NormalizeAngle( record->m_eye_angles.y );
}

void Resolver::ResolveWalk( AimPlayer* data, LagRecord* record ) {
	// apply lby to eyeangles.
	record->m_eye_angles.y = record->m_body;

	// delay body update.
	data->m_body_update = record->m_anim_time + 0.22f;

	// reset stand and body index.
	data->m_stand_index = 0;
	//data->m_stand_index2 = 0;
	data->m_body_index = 0;
	data->m_last_move = 0;
	data->m_unknown_move = 0;

	// copy the last record that this player was walking
	// we need it later on because it gives us crucial data.
	std::memcpy( &data->m_walk_record, record, sizeof( LagRecord ) );
}

float Resolver::GetLBYRotatedYaw( float lby, float yaw )
{
	float delta = math::NormalizedAngle( yaw - lby );
	if ( fabs( delta ) < 25.f )
		return lby;

	if ( delta > 0.f )
		return yaw + 25.f;

	return yaw;
}

bool Resolver::IsYawSideways( Player* entity, float yaw )
{
	auto local_player = core.m_local;
	if ( !local_player )
		return false;

	const auto at_target_yaw = math::CalcAngle( local_player->m_vecOrigin( ), entity->m_vecOrigin( ) ).y;
	const float delta = fabs( math::NormalizedAngle( at_target_yaw - yaw ) );

	return delta > 20.f && delta < 160.f;
}

void Resolver::ResolveYawBruteforce( LagRecord* record, Player* player, AimPlayer* data )
{
	auto local_player = core.m_local;
	if ( !local_player )
		return;

	record->m_mode = Modes::RESOLVE_STAND;

	const float at_target_yaw = math::CalcAngle( player->m_vecOrigin( ), local_player->m_vecOrigin( ) ).y;

	switch ( data->m_stand_index % 3 )
	{
	case 0:
		record->m_eye_angles.y = GetLBYRotatedYaw( player->m_flLowerBodyYawTarget( ), at_target_yaw + 60.f );
		break;
	case 1:
		record->m_eye_angles.y = at_target_yaw + 140.f;
		break;
	case 2:
		record->m_eye_angles.y = at_target_yaw - 75.f;
		break;
	}
}

float Resolver::GetDirectionAngle( int index, Player* player ) {
	const auto left_thickness = core.m_left_thickness[ index ];
	const auto right_thickness = core.m_right_thickness[ index ];
	const auto at_target_angle = core.m_at_target_angle[ index ];

	auto angle = 0.f;

	if ( ( left_thickness >= 350 && right_thickness >= 350 ) || ( left_thickness <= 50 && right_thickness <= 50 ) || ( std::fabs( left_thickness - right_thickness ) <= 7 ) ) {
		angle = math::normalize_float( at_target_angle + 180.f );
	}
	else {
		if ( left_thickness > right_thickness ) {
			angle = math::normalize_float( at_target_angle - 90.f );
		}
		else if ( left_thickness == right_thickness ) {
			angle = math::normalize_float( at_target_angle + 180.f );
		}
		else {
			angle = math::normalize_float( at_target_angle + 90.f );
		}
	}
	return angle;
}

void Resolver::LastMoveLby( LagRecord* record, AimPlayer* data, Player* player )
{
	// for no-spread call a seperate resolver.
	if ( g_menu.main.config.mode.get( ) == 1 ) {
		StandNS( data, record );
		return;
	}

	// pointer for easy access.
	LagRecord* move = &data->m_walk_record;

	// get predicted away angle for the player.
	float away = GetAwayAngle( record );

	C_AnimationLayer* curr = &record->m_layers[ 3 ];
	int act = data->m_player->GetSequenceActivity( curr->m_sequence );

	float diff = math::NormalizedAngle( record->m_body - move->m_body );
	float delta = record->m_anim_time - move->m_anim_time;

	ang_t vAngle = ang_t( 0, 0, 0 );
	math::CalcAngle3( player->m_vecOrigin( ), core.m_local->m_vecOrigin( ), vAngle );

	float flToMe = vAngle.y;

	const float at_target_yaw = math::CalcAngle( core.m_local->m_vecOrigin( ), player->m_vecOrigin( ) ).y;

	const auto freestanding_record = player_resolve_records[ player->index( ) ].m_sAntiEdge;

	// we have a valid moving record.
	if ( move->m_sim_time > 0.f ) {
		vec3_t delta = move->m_origin - record->m_origin;

		// check if moving record is close.
		if ( delta.length( ) <= 128.f ) {
			// indicate that we are using the moving lby.
			data->m_moved = true;
		}
	}

	if ( !data->m_moved ) {

		record->m_mode = Modes::RESOLVE_UNKNOWM;
		ResolveYawBruteforce( record, player, data );

		if ( data->m_body != data->m_old_body )
		{
			record->m_eye_angles.y = record->m_body;

			data->m_body_update = record->m_anim_time + 1.1f;

			iPlayers[ record->m_player->index( ) ] = false;
			record->m_mode = Modes::RESOLVE_BODY;
		}
	}
	else if ( data->m_moved ) {
		float diff = math::NormalizedAngle( record->m_body - move->m_body );
		float delta = record->m_anim_time - move->m_anim_time;

		record->m_mode = Modes::RESOLVE_LASTMOVE;
		const float at_target_yaw = math::CalcAngle( core.m_local->m_vecOrigin( ), player->m_vecOrigin( ) ).y;

		record->m_eye_angles.y = move->m_body;

		if ( data->m_last_move >= 1 )
			ResolveYawBruteforce( record, player, data );

		if ( data->m_body != data->m_old_body )
		{
			record->m_eye_angles.y = record->m_body;
			data->m_body_update = record->m_anim_time + 1.1f;
			iPlayers[ record->m_player->index( ) ] = false;
			record->m_mode = Modes::RESOLVE_BODY;
		}

		// LBY SHOULD HAVE UPDATED HERE.
		if ( record->m_anim_time >= data->m_body_update ) {
			// if we happen to miss then we most likely mispredicted
			if ( data->m_body_index < 1 ) {
				// set angles to current LBY.
				record->m_eye_angles.y = record->m_body;

				data->m_body_update = record->m_anim_time + 1.1f;

				// set the resolve mode.
				iPlayers[ record->m_player->index( ) ] = false;
				record->m_mode = Modes::RESOLVE_BODY;
			}
		}
	}
}

void Resolver::ResolveStand( AimPlayer* data, LagRecord* record ) {
	// for no-spread call a seperate resolver.
	if ( g_menu.main.config.mode.get( ) == 1 ) {
		StandNS( data, record );
		return;
	}

	// get predicted away angle for the player.
	float away = GetAwayAngle( record );

	// pointer for easy access.
	LagRecord* move = &data->m_walk_record;

	// we have a valid moving record.
	if ( move->m_sim_time > 0.f ) {
		vec3_t delta = move->m_origin - record->m_origin;

		// check if moving record is close.
		if ( delta.length( ) <= 128.f ) {
			// indicate that we are using the moving lby.
			data->m_moved = true;
		}
	}

	// a valid moving context was found
	if ( data->m_moved ) {
		float diff = math::NormalizedAngle( record->m_body - move->m_body );
		float delta = record->m_anim_time - move->m_anim_time;

		// it has not been time for this first update yet.
		if ( delta < 0.22f ) {
			// set angles to current LBY.
			record->m_eye_angles.y = move->m_body;

			// set resolve mode.
			record->m_mode = Modes::RESOLVE_STOPPED_MOVING;

			// exit out of the resolver, thats it.
			return;
		}

		// LBY SHOULD HAVE UPDATED HERE.
		else if ( record->m_anim_time >= data->m_body_update ) {
			// only shoot the LBY flick 3 times.
			// if we happen to miss then we most likely mispredicted.
			if ( data->m_body_index <= 3 ) {
				// set angles to current LBY.
				record->m_eye_angles.y = record->m_body;

				// predict next body update.
				data->m_body_update = record->m_anim_time + 1.1f;

				// set the resolve mode.
				record->m_mode = Modes::RESOLVE_BODY;

				return;
			}

			// set to stand1 -> known last move.
			record->m_mode = Modes::RESOLVE_STAND1;

			C_AnimationLayer* curr = &record->m_layers[ 3 ];
			int act = data->m_player->GetSequenceActivity( curr->m_sequence );

			// ok, no fucking update. apply big resolver.
			record->m_eye_angles.y = move->m_body;

			// every third shot do some fuckery.
			if ( !( data->m_stand_index % 3 ) )
				record->m_eye_angles.y += g_csgo.RandomFloat( -35.f, 35.f );

			// jesus we can fucking stop missing can we?
			if ( data->m_stand_index > 6 && act != 980 ) {
				// lets just hope they switched ang after move.
				record->m_eye_angles.y = move->m_body + 180.f;
			}

			// we missed 4 shots.
			else if ( data->m_stand_index > 4 && act != 980 ) {
				// try backwards.
				record->m_eye_angles.y = away + 180.f;
			}

			return;
		}
	}
}
void Resolver::StandNS( AimPlayer* data, LagRecord* record ) {
	// get away angles.
	float away = GetAwayAngle( record );

	switch ( data->m_shots % 8 ) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 90.f;
		break;
	case 2:
		record->m_eye_angles.y = away - 90.f;
		break;

	case 3:
		record->m_eye_angles.y = away + 45.f;
		break;
	case 4:
		record->m_eye_angles.y = away - 45.f;
		break;

	case 5:
		record->m_eye_angles.y = away + 135.f;
		break;
	case 6:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away + 0.f;
		break;

	default:
		break;
	}

	// force LBY to not fuck any pose and do a true bruteforce.
	record->m_body = record->m_eye_angles.y;
}

void Resolver::ResolveAir( AimPlayer* data, LagRecord* record, Player* player ) {
	// for no-spread call a seperate resolver.
	if ( g_menu.main.config.mode.get( ) == 1 ) {
		AirNS( data, record );
		return;
	}

	// else run our matchmaking air resolver.

	// we have barely any speed. 
	// either we jumped in place or we just left the ground.
	// or someone is trying to fool our resolver.
	if ( record->m_velocity.length_2d( ) < 60.f ) {
		// set this for completion.
		// so the shot parsing wont pick the hits / misses up.
		// and process them wrongly.
		record->m_mode = Modes::RESOLVE_LASTMOVE;

		// invoke our stand resolver.
		LastMoveLby( record, data, player );

		// we are done.
		return;
	}

	// try to predict the direction of the player based on his velocity direction.
	// this should be a rough estimation of where he is looking.
	float velyaw = math::rad_to_deg( std::atan2( record->m_velocity.y, record->m_velocity.x ) );

	switch ( data->m_shots % 3 ) {
	case 0:
		record->m_eye_angles.y = velyaw + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = velyaw - 90.f;
		break;

	case 2:
		record->m_eye_angles.y = velyaw + 90.f;
		break;
	}
}

void Resolver::AirNS( AimPlayer* data, LagRecord* record ) {
	// get away angles.
	float away = GetAwayAngle( record );

	switch ( data->m_shots % 9 ) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 150.f;
		break;
	case 2:
		record->m_eye_angles.y = away - 150.f;
		break;

	case 3:
		record->m_eye_angles.y = away + 165.f;
		break;
	case 4:
		record->m_eye_angles.y = away - 165.f;
		break;

	case 5:
		record->m_eye_angles.y = away + 135.f;
		break;
	case 6:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away + 90.f;
		break;
	case 8:
		record->m_eye_angles.y = away - 90.f;
		break;

	default:
		break;
	}
}

void Resolver::ResolvePoses( Player* player, LagRecord* record ) {
	AimPlayer* data = &g_aimbot.m_players[ player->index( ) - 1 ];

	// only do this bs when in air.
	if ( record->m_mode == Modes::RESOLVE_AIR ) {
		// ang = pose min + pose val x ( pose range )

		// lean_yaw
		player->m_flPoseParameter( )[ 2 ] = g_csgo.RandomInt( 0, 4 ) * 0.25f;

		// body_yaw
		player->m_flPoseParameter( )[ 11 ] = g_csgo.RandomInt( 1, 3 ) * 0.25f;
	}
}