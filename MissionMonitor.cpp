// 
// 
// 
#include <ctime>

#include "MissionMonitor.h"
#include "EnumHelper.h"
#include "ArduinoLog.h"
#include "AudioPlayer.h"


MissionMonitor::MissionMonitor( uint32_t secondsBeforeEmergencyStop, GPS_FIX_TYPE lowestGpsFixTpye, AudioPlayer* audioPlayer )
{
	_secondsBeforeEmergencyStop = secondsBeforeEmergencyStop;
	_lowestGpsFixTpye = lowestGpsFixTpye;
	_audioPlayer = audioPlayer;
}

void MissionMonitor::onHeatbeat( mavlink_heartbeat_t mavlink_heartbeat )
{
	_lastHeartbeatTimeMilliseconds = getMissionTime();

	// Check for state change
	if ( mavlink_heartbeat.type == (uint8_t)MAV_TYPE_GROUND_ROVER )
	{
		if ( mavlink_heartbeat.custom_mode != (uint32_t)_roverMode )
		{
			ROVER_MODE roverMode = (ROVER_MODE)mavlink_heartbeat.custom_mode;
			MAV_MODE_FLAG mavModeFlag = (MAV_MODE_FLAG)mavlink_heartbeat.base_mode;

			Log.trace( "Rover mode changed from %s to %s ", EnumHelper::convert( _roverMode ), EnumHelper::convert( roverMode ) );
			play( roverMode );

			_mavModeFlag = mavModeFlag;
			_roverMode = roverMode;

			// The drive mode changed, restart everything
			start();
		}
	}

	if ( _firstHeartbeat == false )
	{
		_firstHeartbeat = true;
		_audioPlayer->play( MAVLINK_GOOD_SOUND );
	}
}

void MissionMonitor::onMissionItemReached( mavlink_mission_item_reached_t mavlink_mission_item_reached )
{
	Log.trace( "Destination reached: %d", mavlink_mission_item_reached.seq );
	_lastProgressMadeTimeMilliseconds = getMissionTime();
}

void MissionMonitor::onNavControllerOutput( mavlink_nav_controller_output_t mavlink_nav_controller )
{
	bool progressMade = false;
	uint32_t missionTime = getMissionTime();

	if ( _lastDistanceToWaypoint == -1 )
	{
		Log.trace( "Distance to new waypoint is %d", mavlink_nav_controller.wp_dist );
		progressMade = true;

	}
	else if ( _lastDistanceToWaypoint == mavlink_nav_controller.wp_dist )
	{
		//Log.trace( "Distance to waypoint is %d Mission Time: %d", mavlink_nav_controller.wp_dist,getMissionTime() );

		// if the last report was going in the wrong direction, don't capture time as progress being made
		if ( !_wrongDirection )
			progressMade = true;
	}
	else if ( _lastDistanceToWaypoint < mavlink_nav_controller.wp_dist )
	{
		// We are making negative progress toward waypoint
		Log.trace( "Distance to waypoint is %d and growing for %d milliseconds", mavlink_nav_controller.wp_dist, missionTime - _lastProgressMadeTimeMilliseconds );
		_wrongDirection = true;
		_wrongDirectionCount += 1;

	}
	else
	{
		Log.trace( "Distance to waypoint is %d and closing Mission Time: %d", mavlink_nav_controller.wp_dist, getMissionTime() );
		progressMade = true;
	}

	_lastDistanceToWaypoint = mavlink_nav_controller.wp_dist;

	if ( progressMade )
	{
		_lastProgressMadeTimeMilliseconds = missionTime;
		_wrongDirection = false;
		_wrongDirectionCount = 0;
	}
}

void MissionMonitor::onMissionCurrent( mavlink_mission_current_t mavlink_mission_current )
{
	if ( _currentWaypointSequenceId != mavlink_mission_current.seq )
	{
		Log.trace( "New destination: %d", mavlink_mission_current.seq );
		_currentWaypointSequenceId = mavlink_mission_current.seq;
		_lastDistanceToWaypoint = -1;
		_lastProgressMadeTimeMilliseconds = getMissionTime();
	}

}

void MissionMonitor::onGPSRawInt( mavlink_gps_raw_int_t mavlink_gps_raw_int )
{
	_gps1FixType = (GPS_FIX_TYPE)mavlink_gps_raw_int.fix_type;

}

void MissionMonitor::onGPS2Raw( mavlink_gps2_raw_t mavlink_gps2_raw )
{
	_gps2FixType = (GPS_FIX_TYPE)mavlink_gps2_raw.fix_type;

}


void MissionMonitor::tick()
{
	evaluateMission();

	if ( !_firstTick )
	{
		_firstTick = true;
		_audioPlayer->play( READY_SOUND );
	}

}

/**
 * @brief
 * This method will monitor the current state of the flight controller. When the flight controller
 * is in AUTO mode, the logic will ensure progress is being made between waypoints. If it detects a problem, this
 * method will call fail mission.
*/
void MissionMonitor::evaluateMission()
{
	uint32_t  missionTime = getMissionTime();
	uint32_t timeDifference = missionTime - _lastProgressMadeTimeMilliseconds;
	uint8_t maxGPSFixType = max( _gps1FixType, _gps2FixType );

	// Check if the rover is in auto mode
	bool isAutoMode = _roverMode == ROVER_MODE_AUTO;

	// Check if the rover is in hold mode
	bool isHoldMode = _roverMode == ROVER_MODE_HOLD;

	// MAVLink is lost if the last heartbeat was received over _secondsBeforeEmergencyStop seconds ago
	bool mavlinkLost = _firstHeartbeat && missionTime - _lastHeartbeatTimeMilliseconds >= (_secondsBeforeEmergencyStop * 1000);

	// GPS is lost if the fix type drops below _lowestGpsFixTpye
	bool gpsLost = maxGPSFixType < _lowestGpsFixTpye;

	// The rover is no longer making progress if the last time it closed the distance to the next waypoint was over _secondsBeforeEmergencyStop seconds
	bool noProgress = _lastProgressMadeTimeMilliseconds != 0 && timeDifference >= (_secondsBeforeEmergencyStop * 1000);


	if ( !_isFailed )
	{

		if ( mavlinkLost )
		{
			// We haven't heard from the flight controller for some time, we can't continue
			Log.trace( "MAVLink lost" );

			failMission();

			_firstHeartbeat = false; // Start looking for first heartbeat again
			_audioPlayer->play( MAVLINK_BAD_SOUND );

		}

		if ( isAutoMode )
		{
			if ( gpsLost )
			{
				// Put the rover in hold mode
				// If the rover does go into hold mode all of the progress counters will be reset by the start() function
				sendModeChange( ROVER_MODE_HOLD );

				Log.trace( "GPS lost, current fix type: %d", maxGPSFixType );

				_audioPlayer->play( GPS_SIGNAL_LOW_SOUND );
			}
			else if ( noProgress )
			{
				// We haven't made progress in the correct direction for some time, stop the rover
				Log.trace( "Last progress time: %d ", round( missionTime - _lastHeartbeatTimeMilliseconds ) );
				failMission();

			}

			if ( _wrongDirectionCount == 2 )
			{
				// wrong direction detected twice, play sound and bump count to avoid play this sound again, its the only thing this count is being used for
				_wrongDirectionCount += 1;
				_audioPlayer->play( WRONG_DIRECTION_SOUND );
			}
		}
		else if ( isHoldMode && !gpsLost )
		{
			// We are in hold mode and gps single is good now
			// Put the rover into auto mode after a gps signal lost
			sendModeChange( ROVER_MODE_AUTO );
		}

	}


}

void MissionMonitor::failMission()
{
	Log.trace( "*************** SHUTDOWN *********************************************" );
	_isFailed = true;
	_servoRelay.powerRelayOff();
	_servoRelay.alarmRelayOn();
	_audioPlayer->play( EMERGENCY_STOP_SOUND );
	Log.trace( "**********************************************************************" );

}

void MissionMonitor::start()
{
	_lastProgressMadeTimeMilliseconds = 0;
	_isFailed = false;
	_wrongDirection = false;
	_wrongDirectionCount = 0;

	_servoRelay.powerRelayOn();
	_servoRelay.alarmRelayOff();


}

void MissionMonitor::play( ROVER_MODE roverMode )
{
	switch ( roverMode )
	{
		case ROVER_MODE_MANUAL:
			_audioPlayer->play( MANUAL_MODE_SOUND );
			break;
		case ROVER_MODE_ACRO:
			_audioPlayer->play( ACRO_MODE_SOUND );
			break;
		case ROVER_MODE_STEERING:
			_audioPlayer->play( STEERING_MODE_SOUND );
			break;
		case ROVER_MODE_HOLD:
			_audioPlayer->play( HOLD_MODE_SOUND );
			break;
		case ROVER_MODE_LOITER:
			_audioPlayer->play( LOITER_MODE_SOUND );
			break;
		case ROVER_MODE_AUTO:
			_audioPlayer->play( AUTO_MODE_SOUND );
			break;
		case ROVER_MODE_RTL:
			_audioPlayer->play( RTL_MODE_SOUND );
			break;
		case ROVER_MODE_SMART_RTL:
			_audioPlayer->play( SRTL_MODE_SOUND );
			break;
		case ROVER_MODE_GUIDED:
			_audioPlayer->play( GUIDED_MODE_SOUND );
			break;
		case ROVER_MODE_INITIALIZING:
			break;
		case ROVER_MODE_ENUM_END:
			break;

	}
}




