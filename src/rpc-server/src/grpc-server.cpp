#include <chrono>
#include <iostream>
#include <queue>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include "jetsonrpc.grpc.pb.h"

#include <cv_bridge/cv_bridge.h>

#include <hero_board/MotorVal.h>
#include <hero_board/SetState.h>
#include <hero_board/SetStateResponse.h>
#include <hero_board/GetState.h>
#include <hero_board/GetStateResponse.h>

#include <image_transport/image_transport.h>
#include <ros/ros.h>
#include <csignal>
#include <sensor_msgs/Imu.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::Status;

using namespace jetsonrpc;
using namespace std;

ros::NodeHandlePtr nh;

// convenient pointer holder and updator
template <typename T>
class PtrHolder {
    public:
    T ptr;
    inline T& operator=(T newPtr) {
        ptr = newPtr;
        return ptr;
    }
    inline void update(T newPtr) {
        ptr = newPtr;
    }
    inline bool operator==(T other) {
        return ptr == other;
    }
    inline T operator->() {
        return ptr;
    }
};

bool SwitchControl(bool state) {
    hero_board::SetStateRequest req;
    req.state = state;

    hero_board::SetStateResponse res;
    bool status = ros::service::call("/set_state", req, res);
    ROS_INFO("%s", res.controlResponse.c_str());
    return status;
}

pair<bool, bool> GetControlState() {
    hero_board::GetStateRequest req;
    hero_board::GetStateResponse res;

    bool status = ros::service::call("/get_state", req, res);
    return {res.state, status};
}

class GreeterServiceImpl final : public JetsonRPC::Service {
    /**
     * publish motor commands send from the client, disable autonomy
     */
    Status SendMotorCmd(ServerContext* context, ServerReader<MotorCmd>* reader, Void* _) override {
        auto state = GetControlState();
        if (!state.second) {
            ROS_ERROR("Failed to get control state");
            return Status::OK;
        }

        auto stateStr = state.first == hero_board::GetStateResponse::MANUAL ? "manual" : "autonomy";
        ROS_INFO("Switching from %s to %s", stateStr, "manual");

        if (!SwitchControl(hero_board::SetStateRequest::MANUAL)) {
            ROS_ERROR("Failed to switch to manual control");
            return Status::OK; 
        }

        MotorCmd cmd;
        hero_board::MotorVal msg; // definitions see hero-serial/Program.cs
        msg.motorval.resize(8);
        auto pub = nh->advertise<decltype(msg)>("/motor/output", 1);
        while (reader->Read(&cmd)) {
            // decode motor values
            uint32_t raw = cmd.values();
            msg.motorval[7] = (raw & 0b11) * 100;  // 0, 100, or 200
            raw >>= 2;
            msg.motorval[6] = (raw & 0b111111) << 2;
            raw >>= 6;
            msg.motorval[5] = (raw & 0b111111) << 2;
            raw >>= 6;
            msg.motorval[4] = (raw & 0b111111) << 2;
            raw >>= 6;
            msg.motorval[3] = msg.motorval[2] = (raw & 0b111111) << 2;
            raw >>= 6;
            msg.motorval[1] = msg.motorval[0] = raw << 2;

            cout << "Client send motor command (decoded): ";
            for (int i = 0; i < 8; i++) {
                cout << (int)msg.motorval[i] << " ";
            }
            cout << endl;
            // publish message
            pub.publish(msg);
        }

        ROS_INFO("Client closes motor command stream, switching to previous control state %s", stateStr);

        if (!SwitchControl(state.first)) {
            ROS_ERROR("Failed to switch to %s", stateStr);
            return Status::OK; 
        }
        return Status::OK;
    }
    Status SendTwist(ServerContext* context, ServerReader<Twist>* reader, Void* _) override {
        Twist twist;

        hero_board::MotorVal msg; // definitions see hero-serial/Program.cs
        msg.motorval.resize(12);
        // reinterpret 12 bytes as 3 floats
        auto raw_data = reinterpret_cast<float*>(msg.motorval.data());

        auto pub = nh->advertise<decltype(msg)>("/motor/output", 1);
        while (reader->Read(&twist)) {
            raw_data[0] = twist.values(0);
            raw_data[1] = twist.values(1);
            raw_data[2] = twist.values(2);

            pub.publish(msg);
        }
        ROS_INFO("Client closes twist stream");
        return Status::OK;
    }
    Status StreamIMU(ServerContext* context, const Rate* _rate, ServerWriter<IMUData>* writer) override {
        IMUData msg;
        msg.mutable_values()->Resize(6, 0); // allocate memory for the underlying buffer

        PtrHolder<sensor_msgs::ImuConstPtr> imuPtr;
        auto sub = nh->subscribe("/camera/imu", 1, &decltype(imuPtr)::update, &imuPtr);
        ros::Rate rate(_rate->rate());
        while (true) {
            ros::spinOnce();
            if (imuPtr == NULL) 
                continue;

            msg.set_values(0, imuPtr->angular_velocity.x);
            msg.set_values(1, imuPtr->angular_velocity.y);
            msg.set_values(2, imuPtr->angular_velocity.z);

            msg.set_values(3, imuPtr->linear_acceleration.x);
            msg.set_values(4, imuPtr->linear_acceleration.y);
            msg.set_values(5, imuPtr->linear_acceleration.z);

            imuPtr = NULL;
            if (!writer->Write(msg))
                break;
            rate.sleep();
        }
        ROS_INFO("Client closes IMU stream");
        return Status::OK;
    }
    Status StreamImage(ServerContext* context, const Rate* _rate, ServerWriter<Image>* writer) override {
        context->set_compression_level(GRPC_COMPRESS_LEVEL_HIGH);

        Image msg;

        PtrHolder<sensor_msgs::CompressedImageConstPtr> imgPtr;
        auto sub = nh->subscribe("/camera/color/image_raw/compressed", 1, &decltype(imgPtr)::update, &imgPtr);
        ros::Rate rate(_rate->rate());
        while (true) {
            ros::spinOnce();  // note: spinOnce is called within the same thread
            if (imgPtr == NULL)
                continue;

            msg.set_encoding(imgPtr->format);
            msg.set_data({reinterpret_cast<const char*>(imgPtr->data.data()), imgPtr->data.size()});
            if (!writer->Write(msg))  // break if client closes the connection
                break;

            imgPtr = NULL;
            rate.sleep();
        }
        ROS_INFO("Client closes image stream");
        return Status::OK;
    }
    Status StreamMotorCurrent(ServerContext* context, const Rate* _rate, ServerWriter<MotorCurrent>* writer) override {
        MotorCurrent current;

        PtrHolder<hero_board::MotorValConstPtr> valPtr;
        auto sub = nh->subscribe("/motor/status", 1, &decltype(valPtr)::update, &valPtr);
        ros::Rate rate(_rate->rate());
        while (true) {
            ros::spinOnce();  // note: spinOnce is called within the same thread
            if (valPtr == NULL)
                continue;

            // reinterpret 8 packed uint8 as uint64
            current.set_values(*reinterpret_cast<const uint64_t*>(valPtr->motorval.data()));
            if (!writer->Write(current))  // break if client closes the connection
                break;

            valPtr = NULL;
            rate.sleep();
        }
        ROS_INFO("Client closes motor current stream");
        return Status::OK;
    }
    Status StreamArmStatus(ServerContext* context, const Rate* _rate, ServerWriter<ArmStatus>* writer) override {
        ArmStatus status;

        PtrHolder<hero_board::MotorValConstPtr> valPtr;
        auto sub = nh->subscribe("/motor/status", 1, &decltype(valPtr)::update, &valPtr);
        ros::Rate rate(_rate->rate());
        while (true) {
            ros::spinOnce();  // note: spinOnce is called within the same thread
            if (valPtr == NULL)
                continue;

            status.set_angle(valPtr->angle);
            status.set_translation(valPtr->translation);
            if (!writer->Write(status))  // break if client closes the connection
                break;

            valPtr = NULL;
            rate.sleep();
        }
        ROS_INFO("Client closes angle stream");
        return Status::OK;
    }

   private:
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    GreeterServiceImpl service;

    ServerBuilder builder;

    // no compression by default
    // builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);

    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    signal(SIGINT, [](int signum) {
        cout << "Keyboard Interrupt" << endl;
        exit(signum);
    });
    server->Wait();
}

int main(int argc, char** argv) {
    ros::init(argc, argv, "image_listener");
    nh = ros::NodeHandlePtr(new ros::NodeHandle);

    RunServer();
    return 0;
}
