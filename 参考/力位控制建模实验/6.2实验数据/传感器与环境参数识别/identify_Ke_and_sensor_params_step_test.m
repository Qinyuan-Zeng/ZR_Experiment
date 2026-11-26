%% identify_Ke_and_sensor_params_fixed_step.m
% ============================================================
% 小位移步进加载实验数据处理：固定时间窗口版
%
% 适用场景：
%   1. 驱动器界面执行固定步进运动；
%   2. 每步保持 4 s；
%   3. 数据开头因点数过多被截断，例如少了前 1.24 s；
%   4. 压力反馈存在静态偏置；
%   5. 采用 50~400 g 的稳态平台点拟合环境刚度 Ke。
%
% 功能：
%   1. 估计压力零偏 F_bias；
%   2. 修正压力反馈 F_corr = F_raw - F_bias；
%   3. 按固定步进时间提取每个稳态平台；
%   4. 采用 50~400 g 稳态点拟合 Ke 和 z_contact；
%   5. 估计传感器等效 delay、rise_time、tau_sensor、N_delay。
%
% 默认列号：
%   1  time(s)
%   3  Vel
%   4  Icmd
%   7  Press Feedback
%   8  Encoder Acc Feedback
%   13 Vcmd
%   14 Ifb
% ============================================================

clear; clc; close all;

%% ===================== 1. 选择数据文件 =====================

[file, path] = uigetfile({'*.csv;*.xlsx;*.xls', ...
    'Data files (*.csv, *.xlsx, *.xls)'}, ...
    '选择步进加载实验数据文件');

if isequal(file, 0)
    error('未选择文件。');
end

filename = fullfile(path, file);
fprintf('读取文件：%s\n', filename);

[~, ~, ext] = fileparts(filename);

if strcmpi(ext, '.csv')
    opts = detectImportOptions(filename);
    opts.VariableNamingRule = 'preserve';
    T = readtable(filename, opts);
else
    T = readtable(filename, 'VariableNamingRule', 'preserve');
end

fprintf('读取到 %d 行，%d 列。\n', height(T), width(T));

%% ===================== 2. 按列号设置 =====================

col_time  = 1;     % time(s)
col_vact  = 3;     % Vel
col_i_cmd = 4;     % Icmd
col_press = 7;     % Press Feedback
col_pos   = 8;     % Encoder Acc Feedback
col_vcmd  = 13;    % Vcmd
col_i_fb  = 14;    % Ifb

%% ===================== 3. 单位与方向设置 =====================

% 如果 Encoder Acc Feedback 是 pulse，且 1 pulse = 1 um：
pos_scale_to_mm = 0.001;

% 如果 Encoder Acc Feedback 已经是 mm，改为：
% pos_scale_to_mm = 1.0;

force_scale_to_g = 1.0;
vel_scale_to_mms = 1.0;
current_scale_to_mA = 1.0;

% 下压方向为正。
% 如果你的数据下压时位置减小，把 pos_sign 改成 -1。
pos_sign = 1.0;
vel_sign = 1.0;
force_sign = 1.0;

%% ===================== 4. 实验时间设置 =====================
% 重点：你的数据保存时截断了开始的 1.24 s。
%
% 情况 A：
%   导出的 time(s) 第一行从 0 开始；
%   说明时间被重新归零，需要加回 1.24 s。
%   time_column_reset_after_truncation = true;
%
% 情况 B：
%   导出的 time(s) 第一行就是 1.2 s 附近；
%   说明时间没有重新归零，不需要加 1.2 s。
%   time_column_reset_after_truncation = false;

data_start_missing_s = 1.24;
time_column_reset_after_truncation = true;

% 原始完整实验中的第一次步进开始时间。
% 你可以根据图中第一次明显位置台阶微调。
% 如果平台点提取错位，优先调整这个值。
first_step_original_s = 5;

% 实验界面参数
step_period_s = 4.0;      % 时间间隔 4000 ms
num_steps = 15;           % 步进次数

% 每个平台取最后 stable_tail_s 秒作为稳态点
stable_tail_s = 1.0;
stable_end_gap_s = 0.10;  % 避免取到下一步运动前边界

%% ===================== 5. 压力偏置与拟合设置 =====================

% 根据你图中 37s 前未接触，取接触前一段估计偏置。
% 这里用的是“原始完整实验时间”。
bias_time_range_s = [5, 35];

% Ke 拟合范围
fit_force_min_g = 50.0;
fit_force_max_g = 400.0;

% 传感器阶跃响应计算
Ts_control = 0.001;       % 仿真/控制周期，s
pre_win_s = 1.0;          % 阶跃前稳态窗口
pre_gap_s = 0.05;         % 避免靠近运动开始
min_step_z_mm = 0.002;    % 最小有效位移阶跃，2 um
min_step_force_g = 2.0;   % 最小有效力变化，2 g

% 平滑窗口
smooth_window = 21;

%% ===================== 6. 读取数据 =====================

t_file  = get_numeric_column_by_index(T, col_time);
z_raw   = get_numeric_column_by_index(T, col_pos);
v_raw   = get_numeric_column_by_index(T, col_vact);
F_raw   = get_numeric_column_by_index(T, col_press);
Icmd_raw = get_numeric_column_by_index(T, col_i_cmd);
Ifb_raw  = get_numeric_column_by_index(T, col_i_fb);
Vcmd_raw = get_numeric_column_by_index(T, col_vcmd);

t_file = double(t_file(:));
z = pos_sign * double(z_raw(:)) * pos_scale_to_mm;
v = vel_sign * double(v_raw(:)) * vel_scale_to_mms;
F_raw_g = force_sign * double(F_raw(:)) * force_scale_to_g;
Icmd = double(Icmd_raw(:)) * current_scale_to_mA;
Ifb = double(Ifb_raw(:)) * current_scale_to_mA;
Vcmd = double(Vcmd_raw(:)) * vel_scale_to_mms;

valid = isfinite(t_file) & isfinite(z) & isfinite(v) & isfinite(F_raw_g);

t_file = t_file(valid);
z = z(valid);
v = v(valid);
F_raw_g = F_raw_g(valid);
Icmd = Icmd(valid);
Ifb = Ifb(valid);
Vcmd = Vcmd(valid);

[t_file, idx] = sort(t_file);
z = z(idx);
v = v(idx);
F_raw_g = F_raw_g(idx);
Icmd = Icmd(idx);
Ifb = Ifb(idx);
Vcmd = Vcmd(idx);

[t_file, idx_unique] = unique(t_file, 'stable');
z = z(idx_unique);
v = v(idx_unique);
F_raw_g = F_raw_g(idx_unique);
Icmd = Icmd(idx_unique);
Ifb = Ifb(idx_unique);
Vcmd = Vcmd(idx_unique);

% 构造“原始完整实验时间轴”
if time_column_reset_after_truncation
    t_exp = t_file + data_start_missing_s;
else
    t_exp = t_file;
end

dt_med = median(diff(t_file));

fprintf('\n========== 数据基本信息 ==========\n');
fprintf('有效数据点数：%d\n', numel(t_file));
fprintf('文件时间范围：%.6f ~ %.6f s\n', min(t_file), max(t_file));
fprintf('修正后实验时间范围：%.6f ~ %.6f s\n', min(t_exp), max(t_exp));
fprintf('中位采样周期：%.8f s\n', dt_med);
fprintf('采样频率约：%.2f Hz\n', 1 / dt_med);
fprintf('位置范围：%.6f ~ %.6f mm\n', min(z), max(z));
fprintf('原始压力范围：%.3f ~ %.3f g\n', min(F_raw_g), max(F_raw_g));

%% ===================== 7. 压力零偏修正 =====================

idx_bias = t_exp >= bias_time_range_s(1) & t_exp <= bias_time_range_s(2);

if sum(idx_bias) < 10
    warning('偏置区间数据点太少，改用当前文件前 2s 数据估计偏置。');
    idx_bias = t_file <= (t_file(1) + 2.0);
end

F_bias = mean(F_raw_g(idx_bias));
noise_std_bias = std(F_raw_g(idx_bias));

F_corr_g = F_raw_g - F_bias;

fprintf('\n========== 压力偏置估计 ==========\n');
fprintf('偏置区间：%.3f ~ %.3f s，按修正后实验时间 t_exp\n', ...
    bias_time_range_s(1), bias_time_range_s(2));
fprintf('F_bias = %.4f g\n', F_bias);
fprintf('接触前噪声 std = %.4f g\n', noise_std_bias);
fprintf('修正后压力范围：%.3f ~ %.3f g\n', min(F_corr_g), max(F_corr_g));

%% ===================== 8. 信号平滑 =====================

smooth_window = make_odd(smooth_window);

if smooth_window >= numel(t_file)
    smooth_window = make_odd(max(5, floor(numel(t_file)/20)));
end

z_s = smoothdata(z, 'sgolay', smooth_window);
F_s = smoothdata(F_corr_g, 'sgolay', smooth_window);

v_from_z = gradient(z_s, t_file);

%% ===================== 9. 按固定步进周期提取稳态平台 =====================

platform_points = table();
step_result = table();

for k = 1:num_steps
    
    % 第 k 次运动开始时间，按原始完整实验时间计算
    t_move_start_exp = first_step_original_s + (k-1) * step_period_s;
    t_next_move_start_exp = first_step_original_s + k * step_period_s;
    
    % 阶跃后平台末端，取下一次运动前
    t_post_end_exp = t_next_move_start_exp - stable_end_gap_s;
    t_post_start_exp = t_post_end_exp - stable_tail_s;
    
    % 阶跃前平台，用于传感器动态计算
    t_pre_end_exp = t_move_start_exp - pre_gap_s;
    t_pre_start_exp = t_pre_end_exp - pre_win_s;
    
    idx_pre = t_exp >= t_pre_start_exp & t_exp <= t_pre_end_exp;
    idx_post = t_exp >= t_post_start_exp & t_exp <= t_post_end_exp;
    
    if sum(idx_post) < 5
        continue;
    end
    
    z1 = mean(z_s(idx_post));
    F1 = mean(F_s(idx_post));
    
    prow = table();
    prow.platform_id = k;
    prow.t_post_start_exp_s = t_post_start_exp;
    prow.t_post_end_exp_s = t_post_end_exp;
    prow.z_mean_mm = z1;
    prow.z_std_mm = std(z_s(idx_post));
    prow.F_mean_g = F1;
    prow.F_std_g = std(F_s(idx_post));
    prow.n = sum(idx_post);
    
    platform_points = [platform_points; prow]; %#ok<AGROW>
    
    % 如果有足够阶跃前数据，也保存阶跃摘要
    if sum(idx_pre) >= 5
        z0 = mean(z_s(idx_pre));
        F0 = mean(F_s(idx_pre));
        
        dz = z1 - z0;
        dF = F1 - F0;
        
        if abs(dz) >= min_step_z_mm && abs(dF) >= min_step_force_g
            srow = table();
            srow.step_id = k;
            srow.t_move_start_exp_s = t_move_start_exp;
            srow.t_next_move_start_exp_s = t_next_move_start_exp;
            
            srow.z0_mm = z0;
            srow.z1_mm = z1;
            srow.dz_mm = dz;
            
            srow.F0_g = F0;
            srow.F1_g = F1;
            srow.dF_g = dF;
            
            srow.F0_std_g = std(F_s(idx_pre));
            srow.F1_std_g = std(F_s(idx_post));
            
            step_result = [step_result; srow]; %#ok<AGROW>
        end
    end
end

fprintf('\n========== 固定时间窗口平台提取 ==========\n');
fprintf('提取到稳态平台点数量：%d\n', height(platform_points));
fprintf('提取到有效阶跃数量：%d\n', height(step_result));

if isempty(platform_points)
    error('没有提取到平台点，请检查 first_step_original_s、step_period_s、num_steps 或时间截断设置。');
end

disp(platform_points(:, {'platform_id','t_post_start_exp_s','t_post_end_exp_s','z_mean_mm','F_mean_g','F_std_g'}));

%% ===================== 10. 采用 50~400g 稳态平台点拟合 Ke =====================

idx_fit = platform_points.F_mean_g >= fit_force_min_g & ...
          platform_points.F_mean_g <= fit_force_max_g;

fprintf('\n50~400 g 内平台点数量：%d\n', sum(idx_fit));

if sum(idx_fit) < 3
    warning('50~400 g 范围内平台点不足 3 个。将输出平台点图，请检查偏置、时间窗口或拟合范围。');
    
    figure('Name', '程序提取的平台点检查');
    scatter(platform_points.z_mean_mm, platform_points.F_mean_g, 80, 'filled');
    grid on;
    xlabel('z / mm');
    ylabel('F_{corr} / g');
    title('程序提取到的稳态平台点');
    
    error('50~400 g 范围内平台点不足 3 个，无法可靠拟合 Ke。');
end

z_fit = platform_points.z_mean_mm(idx_fit);
F_fit = platform_points.F_mean_g(idx_fit);

p = polyfit(z_fit, F_fit, 1);

Ke_g_per_mm = p(1);
b_g = p(2);
z_contact_mm = -b_g / Ke_g_per_mm;

F_fit_pred = polyval(p, z_fit);
fit_residual = F_fit - F_fit_pred;

RMSE_g = sqrt(mean(fit_residual.^2));
SS_res = sum(fit_residual.^2);
SS_tot = sum((F_fit - mean(F_fit)).^2);
R2 = 1 - SS_res / SS_tot;

fprintf('\n========== 环境刚度 Ke 拟合结果 ==========\n');
fprintf('拟合压力范围：%.1f ~ %.1f g\n', fit_force_min_g, fit_force_max_g);
fprintf('参与拟合平台点数：%d\n', sum(idx_fit));
fprintf('Ke_g_per_mm = %.4f g/mm\n', Ke_g_per_mm);
fprintf('b = %.4f g\n', b_g);
fprintf('z_contact = %.6f mm\n', z_contact_mm);
fprintf('RMSE = %.4f g\n', RMSE_g);
fprintf('R^2 = %.5f\n', R2);

%% ===================== 11. 传感器阶跃响应参数 =====================

sensor_result = table();

for k = 1:height(step_result)
    
    step_id = step_result.step_id(k);
    
    F0 = step_result.F0_g(k);
    F1 = step_result.F1_g(k);
    z0 = step_result.z0_mm(k);
    z1 = step_result.z1_mm(k);
    
    dF = F1 - F0;
    dz = z1 - z0;
    
    % 只处理加载方向
    if dF <= 0 || dz <= 0
        continue;
    end
    
    % 只处理 50~400g 附近的阶跃
    if F1 < fit_force_min_g || F0 > fit_force_max_g
        continue;
    end
    
    t_move_start_exp = step_result.t_move_start_exp_s(k);
    t_next_move_start_exp = step_result.t_next_move_start_exp_s(k);
    
    idx_search = t_exp >= (t_move_start_exp - pre_gap_s) & ...
                 t_exp <= (t_next_move_start_exp - stable_end_gap_s);
    
    if sum(idx_search) < 10
        continue;
    end
    
    tt = t_exp(idx_search);
    zz = z_s(idx_search);
    FF = F_s(idx_search);
    
    z10 = z0 + 0.1 * dz;
    F10 = F0 + 0.1 * dF;
    F90 = F0 + 0.9 * dF;
    
    tz10 = crossing_time(tt, zz, z10, sign(dz));
    tF10 = crossing_time(tt, FF, F10, sign(dF));
    tF90 = crossing_time(tt, FF, F90, sign(dF));
    
    if isnan(tz10) || isnan(tF10) || isnan(tF90)
        continue;
    end
    
    delay_time_s = tF10 - tz10;
    rise_time_s = tF90 - tF10;
    
    if rise_time_s <= 0
        continue;
    end
    
    tau_sensor_s = rise_time_s / 2.2;
    bandwidth_Hz = 0.35 / rise_time_s;
    N_delay = round(delay_time_s / Ts_control);
    
    row = table();
    row.step_id = step_id;
    
    row.F0_g = F0;
    row.F1_g = F1;
    row.dF_g = dF;
    
    row.z0_mm = z0;
    row.z1_mm = z1;
    row.dz_mm = dz;
    
    row.tz10_s = tz10;
    row.tF10_s = tF10;
    row.tF90_s = tF90;
    
    row.delay_time_s = delay_time_s;
    row.rise_time_10_90_s = rise_time_s;
    row.tau_sensor_s = tau_sensor_s;
    row.bandwidth_Hz = bandwidth_Hz;
    row.N_delay = N_delay;
    
    sensor_result = [sensor_result; row]; %#ok<AGROW>
end

if isempty(sensor_result)
    warning('没有得到有效传感器阶跃响应参数。Ke 拟合不受影响。');
    
    delay_median = NaN;
    rise_median = NaN;
    tau_median = NaN;
    bw_median = NaN;
    N_delay_median = NaN;
else
    delay_median = median(sensor_result.delay_time_s);
    rise_median = median(sensor_result.rise_time_10_90_s);
    tau_median = median(sensor_result.tau_sensor_s);
    bw_median = median(sensor_result.bandwidth_Hz);
    N_delay_median = round(delay_median / Ts_control);
    
    fprintf('\n========== 传感器等效参数辨识结果 ==========\n');
    fprintf('有效阶跃数量：%d\n', height(sensor_result));
    fprintf('delay median = %.6f s, %.3f ms\n', delay_median, delay_median * 1000);
    fprintf('rise time median = %.6f s, %.3f ms\n', rise_median, rise_median * 1000);
    fprintf('tau_sensor median = %.6f s, %.3f ms\n', tau_median, tau_median * 1000);
    fprintf('bandwidth median = %.2f Hz\n', bw_median);
    fprintf('N_delay median = %d samples, Ts = %.4f s\n', N_delay_median, Ts_control);
end

%% ===================== 12. 保存结果 =====================

out_platform = fullfile(path, 'platform_points_for_Ke_fixed_window.csv');
out_steps = fullfile(path, 'step_summary_fixed_window.csv');
out_sensor = fullfile(path, 'sensor_params_fixed_window.csv');
out_summary = fullfile(path, 'Ke_and_sensor_summary_fixed_window.csv');

writetable(platform_points, out_platform);
writetable(step_result, out_steps);

if ~isempty(sensor_result)
    writetable(sensor_result, out_sensor);
end

summary = table();
summary.data_start_missing_s = data_start_missing_s;
summary.time_column_reset_after_truncation = time_column_reset_after_truncation;
summary.first_step_original_s = first_step_original_s;
summary.step_period_s = step_period_s;
summary.num_steps = num_steps;

summary.F_bias_g = F_bias;
summary.noise_std_bias_g = noise_std_bias;

summary.fit_force_min_g = fit_force_min_g;
summary.fit_force_max_g = fit_force_max_g;
summary.Ke_g_per_mm = Ke_g_per_mm;
summary.z_contact_mm = z_contact_mm;
summary.Ke_RMSE_g = RMSE_g;
summary.Ke_R2 = R2;

summary.sensor_delay_median_s = delay_median;
summary.sensor_rise_median_s = rise_median;
summary.tau_sensor_median_s = tau_median;
summary.bandwidth_median_Hz = bw_median;
summary.N_delay_median = N_delay_median;

writetable(summary, out_summary);

fprintf('\n========== 文件保存完成 ==========\n');
fprintf('平台点：%s\n', out_platform);
fprintf('阶跃摘要：%s\n', out_steps);
if ~isempty(sensor_result)
    fprintf('传感器参数：%s\n', out_sensor);
end
fprintf('汇总：%s\n', out_summary);

%% ===================== 13. 绘图 =====================

figure('Name', '原始信号与零点修正');

subplot(4,1,1);
plot(t_exp, z, 'LineWidth', 1); hold on;
plot(t_exp, z_s, 'LineWidth', 1);
grid on;
ylabel('z / mm');
legend('z raw', 'z smooth');
title('位置反馈');

subplot(4,1,2);
plot(t_exp, F_raw_g, 'LineWidth', 1); hold on;
yline(F_bias, '--', 'F bias');
grid on;
ylabel('F raw / g');
title('原始压力反馈与偏置');

subplot(4,1,3);
plot(t_exp, F_corr_g, 'LineWidth', 1); hold on;
plot(t_exp, F_s, 'LineWidth', 1);
grid on;
ylabel('F corr / g');
legend('F corrected', 'F smooth');
title('零点修正后的压力');

subplot(4,1,4);
plot(t_exp, v, 'LineWidth', 1); hold on;
plot(t_exp, Vcmd, 'LineWidth', 1);
plot(t_exp, v_from_z, 'LineWidth', 1);
grid on;
ylabel('v / mm/s');
xlabel('t_{exp} / s');
legend('Vel', 'Vcmd', 'dz/dt');
title('速度信号');

figure('Name', '固定窗口提取的平台点');

scatter(platform_points.z_mean_mm, platform_points.F_mean_g, 70, 'filled'); hold on;
scatter(z_fit, F_fit, 90, 'filled');

z_line = linspace(min(platform_points.z_mean_mm), max(platform_points.z_mean_mm), 300);
F_line = polyval(p, z_line);
plot(z_line, F_line, 'LineWidth', 2);

grid on;
xlabel('z / mm');
ylabel('F_{corr} / g');
title(sprintf('Ke = %.2f g/mm, z_c = %.5f mm, R^2 = %.4f', ...
    Ke_g_per_mm, z_contact_mm, R2));
legend('全部平台点', '50-400g 拟合点', '线性拟合', 'Location', 'best');

figure('Name', 'Ke 拟合残差');

subplot(2,1,1);
plot(F_fit, fit_residual, 'o-', 'LineWidth', 1);
grid on;
xlabel('F_{fit} / g');
ylabel('Residual / g');
title('拟合残差 vs 压力');

subplot(2,1,2);
plot(z_fit, fit_residual, 'o-', 'LineWidth', 1);
grid on;
xlabel('z / mm');
ylabel('Residual / g');
title('拟合残差 vs 位置');

if ~isempty(sensor_result)
    figure('Name', '传感器阶跃响应参数');

    subplot(3,1,1);
    plot(sensor_result.step_id, sensor_result.delay_time_s * 1000, '-o', 'LineWidth', 1);
    grid on;
    ylabel('delay / ms');
    title('等效延迟');

    subplot(3,1,2);
    plot(sensor_result.step_id, sensor_result.rise_time_10_90_s * 1000, '-o', 'LineWidth', 1);
    grid on;
    ylabel('rise / ms');
    title('10%-90% 上升时间');

    subplot(3,1,3);
    plot(sensor_result.step_id, sensor_result.tau_sensor_s * 1000, '-o', 'LineWidth', 1);
    grid on;
    ylabel('tau / ms');
    xlabel('step id');
    title('等效一阶时间常数');
end

%% ===================== 局部函数 =====================

function x = get_numeric_column_by_index(T, col_idx)
    if col_idx < 1 || col_idx > width(T)
        error('列号 %d 超出范围。当前数据共有 %d 列。', col_idx, width(T));
    end
    
    x_raw = T{:, col_idx};
    
    if isnumeric(x_raw)
        x = x_raw;
        return;
    end
    
    if iscell(x_raw)
        x = nan(size(x_raw));
        for k = 1:numel(x_raw)
            val = x_raw{k};
            if isnumeric(val)
                x(k) = val;
            elseif ischar(val) || isstring(val)
                x(k) = str2double(string(val));
            else
                x(k) = NaN;
            end
        end
        return;
    end
    
    if isstring(x_raw)
        x = str2double(x_raw);
        return;
    end
    
    try
        x = double(x_raw);
    catch
        error('第 %d 列无法转换为数值。', col_idx);
    end
end

function n = make_odd(n)
    n = round(n);
    if mod(n, 2) == 0
        n = n + 1;
    end
    if n < 5
        n = 5;
    end
end

function tc = crossing_time(t, x, target, direction)
    tc = NaN;
    
    if isempty(t)
        return;
    end
    
    if direction >= 0
        idx = find(x >= target, 1, 'first');
    else
        idx = find(x <= target, 1, 'first');
    end
    
    if isempty(idx)
        return;
    end
    
    if idx == 1
        tc = t(1);
        return;
    end
    
    x1 = x(idx-1);
    x2 = x(idx);
    t1 = t(idx-1);
    t2 = t(idx);
    
    if abs(x2 - x1) < eps
        tc = t2;
    else
        tc = t1 + (target - x1) * (t2 - t1) / (x2 - x1);
    end
end