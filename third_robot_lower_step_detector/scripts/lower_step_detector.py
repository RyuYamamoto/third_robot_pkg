#!/usr/bin/env python

## @package third_ronot_lower_step_detector
#  fix original laser scan data to detect down step 
#
#  @author Masaru Morita 

import rospy
import copy
import math
import sys
from std_msgs.msg import String
from sensor_msgs.msg import LaserScan

DEG_TO_RAD = math.pi / 180
RAD_TO_DEG = 180 / math.pi
NAME_NODE = 'lower_step_detector'

class LowerStapDetector():
    ## public constants
    # topic names
    NAME_TOPIC_SUFFIX_TO_PUBLISH = '_fix'
    NAME_TOPIC_LASER_ORI = 'base_scan1'
    NAME_TOPIC_LASER_FIX = NAME_TOPIC_LASER_ORI + NAME_TOPIC_SUFFIX_TO_PUBLISH
    # parameter names
    NAME_PARAM_LASER_INTENSITY = '/lower_step_detector/laser_intensity_max'
    NAME_PARAM_VIRTUAL_LASER_INTENCITY = '/lower_step_detector/virtual_laser_intensity'
    NAME_PARAM_LASER_SCAN_RANGE_DEG = '/lower_step_detector/laser_scan_range_deg'
    NAME_PARAM_DETECT_STEP_ANGLE_MIN_DEG = '/lower_step_detector/detect_step_angle_min_deg'
    # defalut parameter values
    DEFAULT_LASER_INTENSITY_MAX = 3.0
    DEFAULT_VIRTUAL_LASER_INTENSITY = 1.0
    DEDAULT_LASER_SCAN_RANGE_DEG = 180.0
    DEFAULT_DETECT_STEP_ANGLE_MIN_DEG = 40.0

    ## private member variables
    ## @var __laser_ori_sub
    # original laser scan subscriber
    __laser_ori_sub = 0
    ## @var __laser_fix_pub
    # fixed laser scan publisher
    __laser_fix_pub = 0
    ##
    __laser_intensity_max = 0
    ##
    __virtual_laser_intensity = 0
    ##
    __laser_scan_range_deg = 0
    ##
    __detect_step_angle_min_deg = 0
    ##
    __detect_step_angle_max_deg = 0
    ##
    __detect_angle_center_deg = 0

    ## constractor
    def __init__(self):
        rospy.init_node(NAME_NODE, anonymous=False)
        self.load_topic_name_to_sub()
        self.load_rosparam()
        self.__laser_ori_sub = rospy.Subscriber(self.NAME_TOPIC_LASER_ORI, LaserScan, self.on_subscribe_laser_scan)
        self.__laser_fix_pub = rospy.Publisher(self.NAME_TOPIC_LASER_FIX, LaserScan, queue_size = 1)
        # start callback
        rospy.spin()

    ## methods
    def load_topic_name_to_sub(self):
        argvs = sys.argv
        num_arg = len(argvs)
        if num_arg > 1:
            self.NAME_TOPIC_LASER_ORI = argvs[1]
            self.NAME_TOPIC_LASER_FIX = self.NAME_TOPIC_LASER_ORI + self.NAME_TOPIC_SUFFIX_TO_PUBLISH

    def load_rosparam(self):
        self.__laser_intensity_max = rospy.get_param(self.NAME_PARAM_LASER_INTENSITY, self.DEFAULT_LASER_INTENSITY_MAX)
        self.__virtual_laser_intensity = rospy.get_param(self.NAME_PARAM_VIRTUAL_LASER_INTENCITY, self.DEFAULT_VIRTUAL_LASER_INTENSITY)
        self.__laser_scan_range_deg = rospy.get_param(self.NAME_PARAM_LASER_SCAN_RANGE_DEG, self.DEDAULT_LASER_SCAN_RANGE_DEG)
        self.__detect_step_angle_min_deg = rospy.get_param(self.NAME_PARAM_DETECT_STEP_ANGLE_MIN_DEG, self.DEFAULT_DETECT_STEP_ANGLE_MIN_DEG)
        self.__detect_step_angle_max_deg = self.__laser_scan_range_deg - self.__detect_step_angle_min_deg
        self.__detect_angle_center_deg = self.__laser_scan_range_deg / 2.0

    def on_subscribe_laser_scan(self, laser_sensor_msg_ori):
        laser_sensor_msg_fix = copy.deepcopy(laser_sensor_msg_ori)
        # temporary buffer for publish. This is because tuple(type of LaerScan.ranges) type cann't be overwritten.
        tmp_fix_data = len(laser_sensor_msg_ori.ranges)*[0]
        # calculate parameters
        angle_increment = laser_sensor_msg_ori.angle_increment
        detect_index_min = self.__detect_step_angle_min_deg * DEG_TO_RAD / angle_increment
        detect_index_max = self.__detect_step_angle_max_deg * DEG_TO_RAD / angle_increment
        detect_index_mid = self.__detect_angle_center_deg * DEG_TO_RAD / angle_increment

        for i in range(len(laser_sensor_msg_ori.ranges)):
            # copy original data
            tmp_fix_data[i] = copy.deepcopy(laser_sensor_msg_ori.ranges[i])
            # skip when a range cannot detect down step
            if i < detect_index_min or i > detect_index_max:
                continue
            # overwrite only when range can detect down step
            if laser_sensor_msg_ori.ranges[i] > self.__laser_intensity_max:
                angle_curr_rad = i * angle_increment
                angle_curr_deg = angle_curr_rad * RAD_TO_DEG
                print 'detected lower step at %f[degree]!' % angle_curr_deg
                if i < detect_index_mid:
                    theta = angle_curr_rad
                else:
                    theta = math.pi - angle_curr_rad
                tmp_fix_data[i] = self.__virtual_laser_intensity / math.sin(theta)

        # create & register temporary tuple buffer to publishing buffer
        laser_sensor_msg_fix.ranges = tuple(tmp_fix_data)
        # publish fixed laser scan topic
        self.__laser_fix_pub.publish(laser_sensor_msg_fix)

if __name__ == '__main__':
    try:
        print 'start program'
        LowerStapDetector()

    except:
        rospy.loginfo("lower_step_detector finished.")
