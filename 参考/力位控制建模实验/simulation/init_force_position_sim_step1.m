%% init_force_position_sim_step1.m
% 力位控制仿真第一步：工艺流程状态机参数

clear; clc;

%% 仿真步长
Ts = 1e-3;          % 1 ms，先与常见总线/控制周期一致
T_end = 2.0;        % 仿真总时间，可后续调整

%% 行程参数，单位 mm
z_home      = 0.0;     % 初始位置
z_fast_end  = 13.0;    % 快进结束位置
z_v1_end    = 14.0;    % 第一速度结束位置
z_max       = 15.0;    % 最大安全行程

%% 速度参数，单位 mm/s
v1_search   = 5.0;     % 第一速度
v2_search   = 1.0;     % 第二探索速度

%% 位置到达判断
pos_tol = 0.02;        % mm，到达位置容差

%% 接触/停止判断参数
F_th = 10.0;           % g，接触判断力阈值
T_force_confirm = 0.002;   % s，压力阈值持续时间

v_stop_th = 0.05;          % mm/s，速度停止阈值
T_v_confirm = 0.005;       % s，速度低于阈值持续时间

i_stop_th = 1e9;           % 第一版先不启用电流判断，给很大值
T_i_confirm = 0.003;       % s

%% 保压目标
Fd = 200.0;            % g，目标保压力

%% 目标力前馈平滑引入时间，后续模块会用到
T_ramp_force = 0.010;  % s，10 ms

%% 状态编号定义
MODE_OFF    = 0;
MODE_POS    = 1;
MODE_VEL    = 2;
MODE_TORQUE = 3;
MODE_ERROR  = 9;