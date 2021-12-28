#ifndef __OBJECTS_H__
#define __OBJECTS_H__
#include "Headers/Common.hpp"
#include "Headers/Utils.hpp"
#include "Headers/Objects.hpp"
#include "Headers/Publishers.hpp"
#include "Headers/PointClouds.hpp"
#include "Headers/Accumulator.hpp"
#include "Headers/Compensator.hpp"
#include "Headers/Localizator.hpp"
#include "Headers/Mapper.hpp"
#endif

Params Config;

int main(int argc, char** argv) {
    ros::init(argc, argv, "limovelo");
    ros::NodeHandle nh;

    // Get YAML parameters
    bool mapping_online;
    nh.param<double>("delta", Config.delta, 0.025);
    nh.param<int>("rate", Config.rate, (int) 1./Config.delta);
    nh.param<int>("ds_rate", Config.ds_rate, 4);
    nh.param<int>("MAX_NUM_ITERS", Config.MAX_NUM_ITERS, 3);
    nh.param<double>("min_dist", Config.min_dist, 3.);
    nh.param<double>("full_rotation_time", Config.full_rotation_time, 0.1);
    nh.param<double>("empty_lidar_time", Config.empty_lidar_time, 20.);
    nh.param<double>("real_time_delay", Config.real_time_delay, 1.);
    nh.param<std::string>("points_topic", Config.points_topic, "/velodyne_points");
    nh.param<std::string>("imus_topic", Config.imus_topic, "/vectornav/IMU");
    nh.param<bool>("mapping_online", mapping_online, true);
    
    // Objects
    Publishers publish(nh);
    Accumulator& accum = Accumulator::getInstance();
    Compensator comp(publish, Config.delta);
    Mapper& map = Mapper::getInstance();
    Localizator& KF = Localizator::getInstance();

    // Subscribers
    ros::Subscriber lidar_sub = nh.subscribe(
        Config.points_topic, 1000,
        &Accumulator::receive_lidar, &accum
    );

    ros::Subscriber imu_sub = nh.subscribe(
        Config.imus_topic, 1000,
        &Accumulator::receive_imu, &accum
    );

    ros::Rate rate(Config.rate);

    while (ros::ok()) {
        
        if (accum.ready()) {

            // Should be t2 = ros::Time::now() - delay
            double latest_imu_time = accum.BUFFER_I.front().time;
            double t2 = latest_imu_time - Config.real_time_delay; 

            // Refine delta if need to be
            rate = accum.refine_delta(t2);
            double t1 = t2 - accum.delta;

            if (mapping_online or (not mapping_online and map.exists())) {
                // Integrate from t1 to t2
                KF.propagate_to(t2);

                // Compensated pointcloud given a path
                Points points = accum.get_points(t1, t2);
                States path_taken = comp.integrate_imus(t1, t2);
                PointCloud compensated = comp.compensate(path_taken, points);

                // Localize points in map
                KF.update(compensated);
                State Xt2 = KF.latest_state();
                accum.BUFFER_X.push(Xt2);
                publish.state(Xt2, false);

                // Publish compensated
                PointCloud global_compensated = Xt2 * Xt2.I_Rt_L() * compensated;
                publish.pointcloud(global_compensated);

                // Map at the same time (online)
                if (mapping_online) {
                    map.add(global_compensated, t2, false);
                    publish.full_pointcloud(global_compensated);
                }
            }
            
            // Add updated points to map (offline)
            if (not mapping_online and map.hasToMap(t2)) {
                PointCloud full_compensated = comp.compensate(t2 - Config.full_rotation_time, t2);
                PointCloud global_full_compensated = KF.latest_state() * KF.latest_state().I_Rt_L() * full_compensated;
                
                map.add(global_full_compensated, t2, true);
                publish.full_pointcloud(global_full_compensated);
            }

            // Empty too old LiDARs
            accum.empty_lidar(t2 - Config.empty_lidar_time);
        }

        ros::spinOnce();
        rate.sleep();
    }

    return 0;
}