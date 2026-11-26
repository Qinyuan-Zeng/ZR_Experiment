%% init_force_position_sim_step2.m
% 第二步：加载实验一得到的下行静态位置-电流PCHIP查表

clear; clc;

%% 仿真参数
Ts = 1e-3;          % 1 ms
T_end = 2.0;        % 仿真总时间

%% 行程参数，单位 mm
z_home      = 0.0;
z_fast_end  = 13.0;
z_v1_end    = 14.0;
z_max       = 15.0;

%% 速度参数，单位 mm/s
v1_search   = 10.0;
v2_search   = 0.2;

%% 停止判断参数
F_th = 5.0;                 % g
T_force_confirm = 0.001;     % s

v_stop_th = 0.05;            % mm/s
T_v_confirm = 0.005;         % s

Fd = 200.0;                  % g
T_ramp_force = 0.010;        % s

%% 读取下行静态PCHIP查表
lut_file = 'best_down_pchip_lut_anchor_points.csv';
T = readtable(lut_file);

z_anchor_um = T.Position_um(:);
i_anchor_mA = T.Current_down_hold(:);

% 排序，防止表格顺序异常
[z_anchor_um, idx] = sort(z_anchor_um);
i_anchor_mA = i_anchor_mA(idx);

%% 生成稠密PCHIP表
% Simulink中建议用稠密表 + 1-D Lookup Table线性插值
lut_z_um = linspace(min(z_anchor_um), max(z_anchor_um), 501);
lut_i_mA = interp1(z_anchor_um, i_anchor_mA, lut_z_um, 'pchip');

%% 用于Saturation
lut_z_min_um = min(lut_z_um);
lut_z_max_um = max(lut_z_um);

%% 简化Z轴动力学参数
% 注意：这些不是辨识参数，只是为了先跑通仿真流程
Kacc = 20.0;          % mm/s^2 per mA，电流到加速度等效增益
Cv_mA = 1.0;         % mA/(mm/s)，等效粘性阻尼电流
Icoul_mA = 2.0;      % mA，简化库仑摩擦

%% 控制器参数
Kp_pos = 100.0;       % 位置环比例，输出速度
v_fast_max = 100.0;   % 位置快进最大速度，mm/s

Kv = 20.0;           % 速度环比例，输出电流 mA/(mm/s)
i_limit = 5000.0;    % 电流限幅，mA

%% 静态前馈开关
ff_enable = 1;       % 1: 使用静态前馈；0: 不使用