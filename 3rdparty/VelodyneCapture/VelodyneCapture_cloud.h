// VelodyneCapture
//
// VelodyneCapture is the capture class to retrieve the laser data from Velodyne sensors using Boost.Asio and PCAP.
// VelodyneCapture will be able to retrieve lasers infomation about azimuth, vertical and distance that capture at one rotation.
// This class supports direct capture form Velodyne sensors, or capture from PCAP files.
// ( This class only supports VLP-16 and HDL-32E sensor, and not supports Dual Return mode. )
//
// If direct capture from sensors, VelodyneCapture are requires Boost.Asio and its dependent libraries ( Boost.System, Boost.Date_Time, Boost.Regex ).
// Please define HAVE_BOOST in preprocessor.
//
// If capture from PCAP files, VelodyneCapture are requires PCAP.
// Please define HAVE_PCAP in preprocessor.
//
// This source code is licensed under the MIT license. Please see the License in License.txt.
// Copyright (c) 2017 Tsukasa SUGIURA
// t.sugiura0204@gmail.com

#ifndef VELODYNE_CAPTURE_CLOUD_H
#define VELODYNE_CAPTURE_CLOUD_H

#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <functional>
#ifdef HAVE_PCAP
#include <pcap.h>
#endif
#define EPSILON 0.001

namespace velodyne
{
    struct Laser
    {
        double azimuth;
        double vertical;
        unsigned short distance;
        unsigned char intensity;
        unsigned char id;
        long long time;

        const bool operator < ( const struct Laser& laser ){
            if( azimuth == laser.azimuth ){
                return id < laser.id;
            }
            else{
                return azimuth < laser.azimuth;
            }
        }
    };

    template<typename PointT>
    class VelodyneCapture
    {
        typedef pcl::PointCloud<PointT> PointCloudT;
        typedef boost::shared_ptr<pcl::PointCloud<PointT>> PointCloudPtrT;
        protected:
            #ifdef HAVE_PCAP
            pcap_t* pcap = nullptr;
            std::string filename = "";
            #endif

            std::thread* thread = nullptr;
            std::atomic_bool run = { false };
            std::mutex mutex;
            std::queue<PointCloudPtrT> queue;
            Eigen::Matrix4f *transformMatrixPtr = nullptr;

            int MAX_NUM_LASERS;
            std::vector<double> lut;
            std::vector<double> lut_cos;
            std::vector<double> lut_sin;

            static const int LASER_PER_FIRING = 32;
            static const int FIRING_PER_PKT = 12;

            #pragma pack(push, 1)
            typedef struct LaserReturn
            {
                uint16_t distance;
                uint8_t intensity;
            } LaserReturn;
            #pragma pack(pop)

            struct FiringData
            {
                uint16_t blockIdentifier;
                uint16_t rotationalPosition;
                LaserReturn laserReturns[LASER_PER_FIRING];
            };

            struct DataPacket
            {
                FiringData firingData[FIRING_PER_PKT];
                uint32_t gpsTimestamp;
                uint8_t mode;
                uint8_t sensorType;
            };

        public:
            // Constructor
            VelodyneCapture()
            {
            };

            #ifdef HAVE_PCAP
            // Constructor ( capture from PCAP )
            VelodyneCapture( const std::string& filename )
            {
                open( filename );
            };
            #endif

            // Destructor
            ~VelodyneCapture()
            {
                close();
            };

            #ifdef HAVE_PCAP
            // Open Capture from PCAP
            const bool open( const std::string& filename, const Eigen::Matrix4f &transformMatrix )
            {
                this->transformMatrixPtr = new Eigen::Matrix4f(transformMatrix);
                return open(filename);
            };
            const bool open( const std::string& filename )
            {
                // Check Running
                if( isRun() ){
                    close();
                }

                // Open PCAP File
                char error[PCAP_ERRBUF_SIZE];
                pcap_t* pcap = pcap_open_offline( filename.c_str(), error );
                if( !pcap ){
                    throw std::runtime_error( error );
                    return false;
                }

                // Convert PCAP_NETMASK_UNKNOWN to 0xffffffff
                struct bpf_program filter;
                std::ostringstream oss;
                if( pcap_compile( pcap, &filter, oss.str().c_str(), 0, 0xffffffff ) == -1 ){
                    throw std::runtime_error( pcap_geterr( pcap ) );
                    return false;
                }

                if( pcap_setfilter( pcap, &filter ) == -1 ){
                    throw std::runtime_error( pcap_geterr( pcap ) );
                    return false;
                }

                this->pcap = pcap;
                this->filename = filename;

                // Start Capture Thread
                run = true;
                thread = new std::thread( std::bind( &VelodyneCapture::capturePCAP, this ) );

                return true;
            };
            #endif

            // Check Open
            const bool isOpen()
            {
                std::lock_guard<std::mutex> lock( mutex );
                return (
                    #ifdef HAVE_PCAP
                    pcap != nullptr
                    #else
                    false
                    #endif
                );
            };

            // Check Run
            const bool isRun()
            {
                // Returns True when Thread is Running or Queue is Not Empty
                std::lock_guard<std::mutex> lock( mutex );
                return ( run || !queue.empty() );
            }

            // Close Capture
            void close()
            {
                run = false;
                // Close Capturte Thread
                while( thread ) {
                    if(thread->joinable()) {
                        thread->join();
                        thread->~thread();
                        delete thread;
                        thread = nullptr;
                    }
                    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                };
                
                #ifdef HAVE_PCAP
                // Close PCAP
                if( pcap ){
                    pcap_close( pcap );
                    pcap = nullptr;
                    filename = "";
                }
                #endif

                std::lock_guard<std::mutex> lock( mutex );
                // Clear Queue
                std::queue<PointCloudPtrT>().swap( queue );
            };

            // Retrieve Capture Data
            void retrieve( PointCloudPtrT& cloud )
            {
                // Pop One Rotation Data from Queue
                if( mutex.try_lock() ){
                    if( !queue.empty() ){
                        cloud = std::move(queue.front());
                        queue.pop();
                    }
                    mutex.unlock();
                }
            };

            // Retrieve Capture Data
            void retrieve_block( PointCloudPtrT& cloud )
            {
                while(true) {
                    if(getQueueSize() == 0) {
                        if(!run) break;
                    } else {
                        std::lock_guard<std::mutex> lock( mutex );
                        cloud = std::move(queue.front());
                        queue.pop();
                        break;;
                    }
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
            };

            // Operator Retrieve Capture Data with Sort
            void operator >> ( PointCloudPtrT& cloud )
            {
                // Retrieve Capture Data
                retrieve( cloud );
            };

            size_t getQueueSize()
            {
                std::lock_guard<std::mutex> lock( mutex );
                return queue.size();
            }

        private:
            #ifdef HAVE_PCAP
            // Capture Thread from PCAP
            void capturePCAP()
            {
                double last_azimuth = 0.0;

                PointCloudPtrT cloud;
                double M_PI_18000 = M_PI / 18000.0;
                float matrix[4][4];
                if(transformMatrixPtr) {
                    for(int i = 0; i < 4; i++) {
                        for(int j = 0; j < 4; j++) {
                            matrix[i][j] = (*transformMatrixPtr)(i,j);
                        }
                    }
                }

                cloud.reset(new PointCloudT());
                while( run ){
                    // Retrieve Header and Data from PCAP
                    struct pcap_pkthdr* header;
                    const unsigned char* data;
                    const int ret = pcap_next_ex( pcap, &header, &data );
                    if( ret <= 0 ){
                        break;
                    }

                    // Check Packet Data Size
                    // Data Blocks ( 100 bytes * 12 blocks ) + Time Stamp ( 4 bytes ) + Factory ( 2 bytes )
                    if( ( header->len - 42 ) != 1206 ){
                        continue;
                    }

                    // Retrieve Unix Time ( microseconds )
                    std::stringstream ss;
                    ss << header->ts.tv_sec << std::setw( 6 ) << std::left << std::setfill( '0' ) << header->ts.tv_usec;
                    const long long unixtime = std::stoll( ss.str() );

                    // Convert to DataPacket Structure ( Cut Header 42 bytes )
                    // Sensor Type 0x21 is HDL-32E, 0x22 is VLP-16
                    const DataPacket* packet = reinterpret_cast<const DataPacket*>( data + 42 );
                    assert( packet->sensorType == 0x21 || packet->sensorType == 0x22 );

                    // Caluculate Interpolated Azimuth
                    double interpolated = 0.0;
                    if( packet->firingData[1].rotationalPosition < packet->firingData[0].rotationalPosition ){
                        interpolated = ( ( packet->firingData[1].rotationalPosition + 36000 ) - packet->firingData[0].rotationalPosition ) / 2.0;
                    }
                    else{
                        interpolated = ( packet->firingData[1].rotationalPosition - packet->firingData[0].rotationalPosition ) / 2.0;
                    }

                    // Processing Packet
                    for( int firing_index = 0; firing_index < FIRING_PER_PKT; firing_index++ ){
                        // Retrieve Firing Data
                        const FiringData firing_data = packet->firingData[firing_index];
                        for( int laser_index = 0; laser_index < LASER_PER_FIRING; laser_index++ ){
                            // Retrieve Rotation Azimuth
                            double azimuth = static_cast<double>( firing_data.rotationalPosition );

                            int laser_index_modulus = laser_index % MAX_NUM_LASERS;

                            // Interpolate Rotation Azimuth
                            if( laser_index >= MAX_NUM_LASERS )
                            {
                                azimuth += interpolated;
                            }

                            // Reset Rotation Azimuth
                            if( azimuth >= 36000 )
                            {
                                azimuth -= 36000;
                            }

                            // Complete Retrieve Capture One Rotation Data
                            if( last_azimuth > azimuth ){
                                // Push One Rotation Data to Queue
                                cloud->header.stamp = unixtime;
                                cloud->width = static_cast<uint32_t>( cloud->points.size() );
                                cloud->height = 1;
                                mutex.lock();
                                queue.push( cloud );
                                mutex.unlock();
                                cloud.reset(new PointCloudT());
                                #ifdef MAX_QUEUE_SIZE
                                while(run && !(getQueueSize() < MAX_QUEUE_SIZE)) {
                                    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
                                }
                                #endif
                            }
                            if( firing_data.laserReturns[laser_index_modulus].distance < EPSILON ){
                                continue;
                            }
                            //Laser laser;
                            //laser.azimuth = azimuth / 100.0;
                            //laser.vertical = lut[laser_index_modulus];
                            //laser.distance = firing_data.laserReturns[laser_index_modulus].distance * 2.0;

                            //not used
                            //laser.intensity = firing_data.laserReturns[laser_index_modulus].intensity;
                            //laser.id = static_cast<unsigned char>( laser_index_modulus );
                            //#ifdef HAVE_GPSTIME
                            //laser.time = packet->gpsTimestamp;
                            //#else
                            //laser.time = unixtime;
                            //#endif
                            PointT p1,p2;
                            p1.x = static_cast<float>( ( firing_data.laserReturns[laser_index_modulus].distance * 2.0 * lut_cos[laser_index_modulus] ) * std::sin( azimuth * M_PI_18000 ) );
                            p1.y = static_cast<float>( ( firing_data.laserReturns[laser_index_modulus].distance * 2.0 * lut_cos[laser_index_modulus] ) * std::cos( azimuth * M_PI_18000 ) );
                            p1.z = static_cast<float>( ( firing_data.laserReturns[laser_index_modulus].distance * 2.0 * lut_sin[laser_index_modulus] ) );
                        
                            if(transformMatrixPtr) {
                                p2.x = static_cast<float>( matrix[0][0] * p1.x + matrix[0][1] * p1.y + matrix[0][2] * p1.z + matrix[0][3] );
                                p2.y = static_cast<float>( matrix[1][0] * p1.x + matrix[1][1] * p1.y + matrix[1][2] * p1.z + matrix[1][3] );
                                p2.z = static_cast<float>( matrix[2][0] * p1.x + matrix[2][1] * p1.y + matrix[2][2] * p1.z + matrix[2][3] );
                                p1 = p2;
                            }

                            cloud->points.push_back( p1 );

                            // Update Last Rotation Azimuth
                            last_azimuth = azimuth;

                        }
                    }
                }

                run = false;
            };
            #endif
    };

    template<typename PointT>
    class VLP16Capture : public VelodyneCapture<PointT>
    {
        private:
            static const int MAX_NUM_LASERS = 16;
            const std::vector<double> lut = { -15.0, 1.0, -13.0, 3.0, -11.0, 5.0, -9.0, 7.0, -7.0, 9.0, -5.0, 11.0, -3.0, 13.0, -1.0, 15.0 };
            std::vector<double> lut_cos = { -15.0, 1.0, -13.0, 3.0, -11.0, 5.0, -9.0, 7.0, -7.0, 9.0, -5.0, 11.0, -3.0, 13.0, -1.0, 15.0 };
            std::vector<double> lut_sin = { -15.0, 1.0, -13.0, 3.0, -11.0, 5.0, -9.0, 7.0, -7.0, 9.0, -5.0, 11.0, -3.0, 13.0, -1.0, 15.0 };

        public:
            VLP16Capture() : VelodyneCapture<PointT>()
            {
                initialize();
            };

            #ifdef HAVE_PCAP
            VLP16Capture( const std::string& filename ) : VelodyneCapture<PointT>( filename )
            {
                initialize();
            };
            #endif

            ~VLP16Capture()
            {
            };

        private:
            void initialize()
            {
                std::transform(lut.begin(), lut.end(), lut_cos.begin(), [](auto &a){return std::cos( a * M_PI / 180.0); });
                std::transform(lut.begin(), lut.end(), lut_sin.begin(), [](auto &a){return std::sin( a * M_PI / 180.0); });
                VelodyneCapture<PointT>::MAX_NUM_LASERS = MAX_NUM_LASERS;
                VelodyneCapture<PointT>::lut = lut;
                VelodyneCapture<PointT>::lut_cos = lut_cos;
                VelodyneCapture<PointT>::lut_sin = lut_sin;
            };
    };

    template<typename PointT>
    class HDL32ECapture : public VelodyneCapture<PointT>
    {
        private:
            static const int MAX_NUM_LASERS = 32;
            const std::vector<double> lut = { -30.67, -9.3299999, -29.33, -8.0, -28, -6.6700001, -26.67, -5.3299999, -25.33, -4.0, -24.0, -2.6700001, -22.67, -1.33, -21.33, 0.0, -20.0, 1.33, -18.67, 2.6700001, -17.33, 4.0, -16, 5.3299999, -14.67, 6.6700001, -13.33, 8.0, -12.0, 9.3299999, -10.67, 10.67 };

        public:
            HDL32ECapture() : VelodyneCapture<PointT>()
            {
                initialize();
            };

            #ifdef HAVE_PCAP
            HDL32ECapture( const std::string& filename ) : VelodyneCapture<PointT>( filename )
            {
                initialize();
            };
            #endif

            ~HDL32ECapture()
            {
            };

        private:
            void initialize()
            {
                VelodyneCapture<PointT>::MAX_NUM_LASERS = MAX_NUM_LASERS;
                VelodyneCapture<PointT>::lut = lut;
            };
    };
}

#endif
