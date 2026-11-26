%% 批量处理恒速上下行实验数据：多速度、多组重复实验
% 功能：
%   1. 自动处理多个速度、多个重复实验文件；
%   2. 对每个文件提取下行/上行恒速段；
%   3. 计算 i_fric = (i_down - i_up)/2；
%   4. 计算 i_avg = (i_down + i_up)/2；
%   5. 汇总每个速度下三组实验的均值和标准差；
%   6. 拟合摩擦模型 i_fric(V) = b0 + b1*V；
%   7. 保存所有结果。
%
% 文件命名默认格式：
%   50_1.csv, 50_2.csv, 50_3.csv
%   80_1.csv, 80_2.csv, 80_3.csv
%   ...
%
% 如果文件命名不同，请修改 filename_pattern。

clear; clc; close all;

fprintf('=== 多速度恒速上下行实验批处理开始 ===\n');

%% ===================== 0. 用户配置区 =====================

% 数据文件所在文件夹
data_folder = pwd;

% 文件名格式
% 默认：速度_组号.csv，例如 180_1.csv
filename_pattern = '%d_%d.csv';

% 速度列表，单位 mm/s
V_list = [50, 80, 120, 180, 300];

% 每个速度的重复次数
rep_list = 1:3;

% 数据列号，根据你的 CSV 实际情况调整
col_time     = 1;   % 时间，s
col_velocity = 3;   % 实际速度，mm/s
col_current  = 4;   % 电流指令或实际电流
col_force    = 7;   % 力反馈，N
col_position = 8;   % 位置反馈

% 位置单位设置
% 如果第8列位置单位是 mm，设为 'mm'
% 如果第8列位置单位是 um，设为 'um'
position_unit = 'um';

% 统一内部处理位置单位为 um
% 速度仍使用 mm/s
switch lower(position_unit)
    case 'mm'
        pos_to_um = 1000;
    case 'um'
        pos_to_um = 1;
    otherwise
        error('position_unit 只能设置为 mm 或 um');
end

% 方向设置
% 如果位置数据经过转换后，压头向下运动时位置增大，则为 true
% 如果压头向下运动时位置减小，则设为 false，并在程序内自动取反
down_positive_position = true;

% 如果电流正方向不是向下推力，可以设为 true 取反
reverse_current_sign = false;

% 恒速段提取参数
cfg = struct();

% 速度容差设置
% 实际容差 = max(V_tolerance_abs, V_tolerance_ratio * V_target)
cfg.V_tolerance_abs = 6;        % mm/s
cfg.V_tolerance_ratio = 0.05;   % 5%

% 力阈值，用于排除接触或异常受力数据
cfg.force_threshold = 20;       % N

% 只保留恒速候选点中间位置段，避免加减速污染
cfg.keep_ratio = 0.80;          % 保留中间 80%

% 最少恒速点数
cfg.min_const_points = 20;

% 插值网格点数
cfg.num_grid_points = 1000;

% 是否对电流做轻微平滑
cfg.enable_current_smooth = false;
cfg.current_smooth_window = 5;

% 是否对速度做轻微平滑用于恒速段判断
cfg.enable_velocity_smooth = false;
cfg.velocity_smooth_window = 5;

% 是否保存每个文件的详细图
plot_each_file = false;

% 是否保存每个速度的汇总图
plot_each_speed_summary = true;

% 输出文件夹
output_folder = fullfile(pwd, 'batch_friction_results');
if ~exist(output_folder, 'dir')
    mkdir(output_folder);
end

%% ===================== 1. 批量处理所有文件 =====================

all_results = struct([]);
result_counter = 0;

for iV = 1:length(V_list)

    V_target = V_list(iV);
    fprintf('\n========================================\n');
    fprintf('开始处理速度 V = %d mm/s\n', V_target);
    fprintf('========================================\n');

    for iRep = 1:length(rep_list)

        rep_id = rep_list(iRep);
        filename = sprintf(filename_pattern, V_target, rep_id);
        filepath = fullfile(data_folder, filename);

        fprintf('\n--- 处理文件: %s ---\n', filename);

        if ~exist(filepath, 'file')
            warning('文件不存在，跳过: %s', filepath);
            continue;
        end

        try
            one_result = process_one_const_speed_file( ...
                filepath, ...
                V_target, ...
                rep_id, ...
                col_time, ...
                col_velocity, ...
                col_current, ...
                col_force, ...
                col_position, ...
                pos_to_um, ...
                down_positive_position, ...
                reverse_current_sign, ...
                cfg, ...
                plot_each_file, ...
                output_folder);

            result_counter = result_counter + 1;
            all_results(result_counter).filename = filename;
            all_results(result_counter).filepath = filepath;
            all_results(result_counter).V_target = V_target;
            all_results(result_counter).rep_id = rep_id;
            all_results(result_counter).success = true;
            all_results(result_counter).result = one_result;

            fprintf('处理成功: V=%d, rep=%d\n', V_target, rep_id);
            fprintf('  i_fric_mean = %.4f\n', one_result.friction.i_fric_mean);
            fprintf('  i_fric_std  = %.4f\n', one_result.friction.i_fric_std);
            fprintf('  i_avg_mean  = %.4f\n', one_result.friction.i_avg_mean);
            fprintf('  下行平均速度 = %.3f mm/s\n', one_result.const_down.velocity_mean);
            fprintf('  上行平均速度 = %.3f mm/s\n', one_result.const_up.velocity_mean);

        catch ME
            warning('处理失败: %s\n错误信息: %s', filename, ME.message);

            result_counter = result_counter + 1;
            all_results(result_counter).filename = filename;
            all_results(result_counter).filepath = filepath;
            all_results(result_counter).V_target = V_target;
            all_results(result_counter).rep_id = rep_id;
            all_results(result_counter).success = false;
            all_results(result_counter).error_message = ME.message;
        end
    end
end

if isempty(all_results)
    error('没有任何文件被成功读取或处理。请检查文件路径和命名格式。');
end

%% ===================== 2. 汇总每个速度的结果 =====================

summary_rows = [];

for iV = 1:length(V_list)

    V_target = V_list(iV);

    idx_this_V = arrayfun(@(s) s.V_target == V_target && s.success, all_results);

    if ~any(idx_this_V)
        warning('速度 V=%d mm/s 没有成功结果。', V_target);
        continue;
    end

    res_this = all_results(idx_this_V);

    n_rep_valid = length(res_this);

    i_fric_mean_list = zeros(n_rep_valid, 1);
    i_fric_std_list  = zeros(n_rep_valid, 1);
    i_avg_mean_list  = zeros(n_rep_valid, 1);
    v_down_mean_list = zeros(n_rep_valid, 1);
    v_up_mean_list   = zeros(n_rep_valid, 1);
    n_down_list      = zeros(n_rep_valid, 1);
    n_up_list        = zeros(n_rep_valid, 1);

    for k = 1:n_rep_valid
        r = res_this(k).result;

        i_fric_mean_list(k) = r.friction.i_fric_mean;
        i_fric_std_list(k)  = r.friction.i_fric_std;
        i_avg_mean_list(k)  = r.friction.i_avg_mean;

        v_down_mean_list(k) = r.const_down.velocity_mean;
        v_up_mean_list(k)   = r.const_up.velocity_mean;

        n_down_list(k) = r.const_down.num_points;
        n_up_list(k)   = r.const_up.num_points;
    end

    row = table( ...
        V_target, ...
        n_rep_valid, ...
        mean(i_fric_mean_list), ...
        std(i_fric_mean_list), ...
        mean(i_fric_std_list), ...
        mean(i_avg_mean_list), ...
        std(i_avg_mean_list), ...
        mean(v_down_mean_list), ...
        std(v_down_mean_list), ...
        mean(v_up_mean_list), ...
        std(v_up_mean_list), ...
        mean(n_down_list), ...
        mean(n_up_list), ...
        'VariableNames', { ...
            'V_target_mm_s', ...
            'NumValidReps', ...
            'i_fric_mean_avg', ...
            'i_fric_mean_std_across_reps', ...
            'i_fric_within_file_std_avg', ...
            'i_avg_mean_avg', ...
            'i_avg_mean_std_across_reps', ...
            'v_down_mean_avg', ...
            'v_down_mean_std', ...
            'v_up_mean_avg', ...
            'v_up_mean_std', ...
            'n_down_avg', ...
            'n_up_avg' ...
        });

    summary_rows = [summary_rows; row];
end

fprintf('\n===== 多速度汇总结果 =====\n');
disp(summary_rows);

%% ===================== 3. 拟合摩擦模型 i_fric(V) =====================

V_fit = summary_rows.V_target_mm_s;
i_fric_fit_data = summary_rows.i_fric_mean_avg;
i_fric_fit_std  = summary_rows.i_fric_mean_std_across_reps;

% 线性摩擦模型：i_fric = b0 + b1 * V
p_linear = polyfit(V_fit, i_fric_fit_data, 1);
b1 = p_linear(1);
b0 = p_linear(2);

i_fric_pred_linear = polyval(p_linear, V_fit);

SS_res = sum((i_fric_fit_data - i_fric_pred_linear).^2);
SS_tot = sum((i_fric_fit_data - mean(i_fric_fit_data)).^2);
R2_linear = 1 - SS_res / SS_tot;
RMSE_linear = sqrt(mean((i_fric_fit_data - i_fric_pred_linear).^2));

fprintf('\n===== 摩擦模型拟合结果 =====\n');
fprintf('线性模型: i_fric(V) = %.6f * V + %.6f\n', b1, b0);
fprintf('R2   = %.5f\n', R2_linear);
fprintf('RMSE = %.5f\n', RMSE_linear);

%% ===================== 4. 绘制汇总图 =====================

figure('Name', '多速度摩擦电流汇总与拟合', ...
    'Position', [100, 80, 1200, 800]);

subplot(2,2,1);

errorbar(V_fit, i_fric_fit_data, i_fric_fit_std, 'bo', ...
    'MarkerFaceColor', 'b', ...
    'LineWidth', 1.5, ...
    'DisplayName', '实验均值 ± 重复标准差');
hold on;

V_dense = linspace(min(V_fit), max(V_fit), 300)';
i_dense_linear = polyval(p_linear, V_dense);

plot(V_dense, i_dense_linear, 'r-', ...
    'LineWidth', 2, ...
    'DisplayName', sprintf('线性拟合: i=%.4fV%+.4f', b1, b0));

xlabel('速度 V (mm/s)');
ylabel('摩擦电流 i_{fric}');
title('摩擦电流-速度关系');
grid on;
legend('Location', 'best');

text(0.03, 0.95, ...
    sprintf('R^2=%.4f\nRMSE=%.4f', R2_linear, RMSE_linear), ...
    'Units', 'normalized', ...
    'VerticalAlignment', 'top', ...
    'BackgroundColor', 'white', ...
    'EdgeColor', [0.5 0.5 0.5]);

subplot(2,2,2);

bar(V_fit, i_fric_fit_std);
xlabel('速度 V (mm/s)');
ylabel('重复实验标准差');
title('每个速度下三组实验的 i_{fric} 离散程度');
grid on;

subplot(2,2,3);

plot(V_fit, summary_rows.v_down_mean_avg, 'bo-', ...
    'LineWidth', 1.5, ...
    'MarkerFaceColor', 'b', ...
    'DisplayName', '下行实际平均速度');
hold on;

plot(V_fit, abs(summary_rows.v_up_mean_avg), 'rv-', ...
    'LineWidth', 1.5, ...
    'MarkerFaceColor', 'r', ...
    'DisplayName', '上行实际平均速度绝对值');

plot(V_fit, V_fit, 'k--', ...
    'LineWidth', 1.2, ...
    'DisplayName', '理想速度');

xlabel('目标速度 V (mm/s)');
ylabel('实际平均速度幅值 (mm/s)');
title('上下行实际速度对称性检查');
grid on;
legend('Location', 'best');

subplot(2,2,4);

v_sym_error = summary_rows.v_down_mean_avg - abs(summary_rows.v_up_mean_avg);

plot(V_fit, v_sym_error, 'ko-', ...
    'LineWidth', 1.5, ...
    'MarkerFaceColor', 'k');

xlabel('目标速度 V (mm/s)');
ylabel('v_{down} - |v_{up}| (mm/s)');
title('上下行速度不对称误差');
grid on;
yline(0, 'r--');

sgtitle('多速度恒速上下行实验汇总分析');

%% ===================== 5. 可选：每个速度画三组 i_fric(x) 对比 =====================

if plot_each_speed_summary

    for iV = 1:length(V_list)

        V_target = V_list(iV);
        idx_this_V = arrayfun(@(s) s.V_target == V_target && s.success, all_results);

        if ~any(idx_this_V)
            continue;
        end

        res_this = all_results(idx_this_V);

        figure('Name', sprintf('V=%d mm/s 三组重复实验对比', V_target), ...
            'Position', [120, 80, 1200, 600]);

        subplot(1,2,1);
        hold on;

        for k = 1:length(res_this)
            r = res_this(k).result;
            plot(r.friction.pos_grid_um, r.friction.i_fric, ...
                'LineWidth', 1.2, ...
                'DisplayName', sprintf('rep %d', res_this(k).rep_id));
        end

        xlabel('位置 x (um)');
        ylabel('i_{fric}');
        title(sprintf('V=%d mm/s: 三组 i_{fric}(x)', V_target));
        grid on;
        legend('Location', 'best');

        subplot(1,2,2);
        hold on;

        for k = 1:length(res_this)
            r = res_this(k).result;
            plot(r.friction.pos_grid_um, r.friction.i_avg, ...
                'LineWidth', 1.2, ...
                'DisplayName', sprintf('rep %d', res_this(k).rep_id));
        end

        xlabel('位置 x (um)');
        ylabel('i_{avg}');
        title(sprintf('V=%d mm/s: 三组 i_{avg}(x)', V_target));
        grid on;
        legend('Location', 'best');
    end
end

%% ===================== 6. 保存所有结果 =====================

batch_results = struct();
batch_results.V_list = V_list;
batch_results.rep_list = rep_list;
batch_results.filename_pattern = filename_pattern;
batch_results.all_results = all_results;
batch_results.summary = summary_rows;
batch_results.friction_model.linear.p = p_linear;
batch_results.friction_model.linear.b0 = b0;
batch_results.friction_model.linear.b1 = b1;
batch_results.friction_model.linear.R2 = R2_linear;
batch_results.friction_model.linear.RMSE = RMSE_linear;
batch_results.config = cfg;

save(fullfile(output_folder, 'batch_friction_results.mat'), 'batch_results');

writetable(summary_rows, fullfile(output_folder, 'batch_friction_summary.csv'));

fprintf('\n结果已保存到文件夹:\n%s\n', output_folder);
fprintf('  batch_friction_results.mat\n');
fprintf('  batch_friction_summary.csv\n');

fprintf('\n=== 多速度恒速上下行实验批处理完成 ===\n');

%% ========================================================================
%% 局部函数：处理单个恒速上下行实验文件
%% ========================================================================

function result = process_one_const_speed_file( ...
    filepath, ...
    V_target, ...
    rep_id, ...
    col_time, ...
    col_velocity, ...
    col_current, ...
    col_force, ...
    col_position, ...
    pos_to_um, ...
    down_positive_position, ...
    reverse_current_sign, ...
    cfg, ...
    plot_each_file, ...
    output_folder)

    data = readtable(filepath);

    time = data{:, col_time};
    velocity = data{:, col_velocity};
    current_cmd = data{:, col_current};
    force_fb = data{:, col_force};
    position_raw = data{:, col_position};

    time = time(:);
    velocity = velocity(:);
    current_cmd = current_cmd(:);
    force_fb = force_fb(:);
    position_raw = position_raw(:);

    valid_raw = isfinite(time) & isfinite(velocity) & isfinite(current_cmd) & ...
                isfinite(force_fb) & isfinite(position_raw);

    time = time(valid_raw);
    velocity = velocity(valid_raw);
    current_cmd = current_cmd(valid_raw);
    force_fb = force_fb(valid_raw);
    position_raw = position_raw(valid_raw);

    [time, sort_idx] = sort(time);
    velocity = velocity(sort_idx);
    current_cmd = current_cmd(sort_idx);
    force_fb = force_fb(sort_idx);
    position_raw = position_raw(sort_idx);

    % 位置统一转换为 um
    position_um = position_raw * pos_to_um;

    % 位置方向统一为向下为正
    if ~down_positive_position
        position_um = -position_um;
        velocity = -velocity;
    end

    % 电流方向可选取反
    if reverse_current_sign
        current_cmd = -current_cmd;
    end

    % 可选平滑
    velocity_for_select = velocity;
    current_for_calc = current_cmd;

    if cfg.enable_velocity_smooth
        velocity_for_select = movmean(velocity_for_select, cfg.velocity_smooth_window);
    end

    if cfg.enable_current_smooth
        current_for_calc = movmean(current_for_calc, cfg.current_smooth_window);
    end

    % 检查数据长度
    if length(time) < 20
        error('数据点过少。');
    end

    % 寻找反转点：向下为正时，最大位置为反转点
    [~, idx_turn] = max(position_um);

    if idx_turn <= 5 || idx_turn >= length(time) - 5
        error('反转点检测异常，idx_turn=%d。', idx_turn);
    end

    % 分割下行和上行
    idx_down_all = 1:idx_turn;
    idx_up_all = idx_turn:length(time);

    time_down = time(idx_down_all);
    vel_down = velocity_for_select(idx_down_all);
    current_down = current_for_calc(idx_down_all);
    pos_down = position_um(idx_down_all);
    force_down = force_fb(idx_down_all);

    time_up = time(idx_up_all);
    vel_up = velocity_for_select(idx_up_all);
    current_up = current_for_calc(idx_up_all);
    pos_up = position_um(idx_up_all);
    force_up = force_fb(idx_up_all);

    % 恒速段筛选
    V_tol = max(cfg.V_tolerance_abs, cfg.V_tolerance_ratio * V_target);

    % 下行：速度约 +V_target
    is_down_speed_ok = abs(vel_down - V_target) <= V_tol;
    is_down_force_ok = abs(force_down) < cfg.force_threshold;
    down_candidate_idx = find(is_down_speed_ok & is_down_force_ok);

    % 上行：速度约 -V_target
    is_up_speed_ok = abs(vel_up + V_target) <= V_tol;
    is_up_force_ok = abs(force_up) < cfg.force_threshold;
    up_candidate_idx = find(is_up_speed_ok & is_up_force_ok);

    if isempty(down_candidate_idx)
        error('未找到下行恒速候选点。');
    end

    if isempty(up_candidate_idx)
        error('未找到上行恒速候选点。');
    end

    % 按位置保留中间段，剔除加减速附近
    idx_const_down = keep_middle_position_points(down_candidate_idx, pos_down, cfg.keep_ratio);
    idx_const_up = keep_middle_position_points(up_candidate_idx, pos_up, cfg.keep_ratio);

    if length(idx_const_down) < cfg.min_const_points
        error('下行恒速段点数不足: %d。', length(idx_const_down));
    end

    if length(idx_const_up) < cfg.min_const_points
        error('上行恒速段点数不足: %d。', length(idx_const_up));
    end

    const_time_down = time_down(idx_const_down);
    const_vel_down = vel_down(idx_const_down);
    const_current_down = current_down(idx_const_down);
    const_pos_down = pos_down(idx_const_down);

    const_time_up = time_up(idx_const_up);
    const_vel_up = vel_up(idx_const_up);
    const_current_up = current_up(idx_const_up);
    const_pos_up = pos_up(idx_const_up);

    % 共同位置区间
    common_pos_min = max(min(const_pos_down), min(const_pos_up));
    common_pos_max = min(max(const_pos_down), max(const_pos_up));

    if common_pos_min >= common_pos_max
        error('上下行恒速段位置区间无重叠。');
    end

    pos_grid_um = linspace(common_pos_min, common_pos_max, cfg.num_grid_points)';

    % 为了 interp1 稳定，排序并去重
    [pos_down_unique, current_down_unique] = sort_and_average_duplicate_position( ...
        const_pos_down, const_current_down);

    [pos_up_unique, current_up_unique] = sort_and_average_duplicate_position( ...
        const_pos_up, const_current_up);

    current_down_interp = interp1(pos_down_unique, current_down_unique, pos_grid_um, 'linear', NaN);
    current_up_interp   = interp1(pos_up_unique,   current_up_unique,   pos_grid_um, 'linear', NaN);

    valid_interp = isfinite(current_down_interp) & isfinite(current_up_interp);

    pos_grid_um = pos_grid_um(valid_interp);
    current_down_interp = current_down_interp(valid_interp);
    current_up_interp = current_up_interp(valid_interp);

    if length(pos_grid_um) < 10
        error('有效插值点过少。');
    end

    i_fric = (current_down_interp - current_up_interp) / 2;
    i_avg  = (current_down_interp + current_up_interp) / 2;

    % 结果结构
    result = struct();

    [~, name_only, ext] = fileparts(filepath);
    result.filename = [name_only, ext];
    result.V_target = V_target;
    result.rep_id = rep_id;
    result.V_tolerance = V_tol;

    result.raw.time = time;
    result.raw.velocity = velocity;
    result.raw.current = current_cmd;
    result.raw.force = force_fb;
    result.raw.position_um = position_um;

    result.idx_turn = idx_turn;

    result.const_down.time = const_time_down;
    result.const_down.velocity = const_vel_down;
    result.const_down.current = const_current_down;
    result.const_down.position_um = const_pos_down;
    result.const_down.num_points = length(const_pos_down);
    result.const_down.velocity_mean = mean(const_vel_down);
    result.const_down.velocity_std = std(const_vel_down);

    result.const_up.time = const_time_up;
    result.const_up.velocity = const_vel_up;
    result.const_up.current = const_current_up;
    result.const_up.position_um = const_pos_up;
    result.const_up.num_points = length(const_pos_up);
    result.const_up.velocity_mean = mean(const_vel_up);
    result.const_up.velocity_std = std(const_vel_up);

    result.friction.pos_grid_um = pos_grid_um;
    result.friction.current_down = current_down_interp;
    result.friction.current_up = current_up_interp;
    result.friction.i_fric = i_fric;
    result.friction.i_avg = i_avg;
    result.friction.i_fric_mean = mean(i_fric);
    result.friction.i_fric_std = std(i_fric);
    result.friction.i_fric_min = min(i_fric);
    result.friction.i_fric_max = max(i_fric);
    result.friction.i_avg_mean = mean(i_avg);
    result.friction.i_avg_std = std(i_avg);

    % 可选：单文件图
    if plot_each_file
        fig = figure('Name', sprintf('%s 单文件处理结果', result.filename), ...
            'Position', [100, 100, 1200, 750]);

        subplot(2,2,1);
        plot(time, position_um, 'k-');
        hold on;
        xline(time(idx_turn), 'r--', '反转点');
        xlabel('时间 (s)');
        ylabel('位置 (um)');
        title('位置与反转点');
        grid on;

        subplot(2,2,2);
        plot(time, velocity, 'k-');
        hold on;
        plot(const_time_down, const_vel_down, 'bo');
        plot(const_time_up, const_vel_up, 'ro');
        yline(V_target, 'b--');
        yline(-V_target, 'r--');
        xlabel('时间 (s)');
        ylabel('速度 (mm/s)');
        title('恒速段选择');
        grid on;

        subplot(2,2,3);
        plot(pos_grid_um, current_down_interp, 'b-', 'DisplayName', '下行');
        hold on;
        plot(pos_grid_um, current_up_interp, 'r-', 'DisplayName', '上行');
        xlabel('位置 (um)');
        ylabel('电流');
        title('上下行电流插值对比');
        grid on;
        legend('Location', 'best');

        subplot(2,2,4);
        yyaxis left;
        plot(pos_grid_um, i_fric, 'k-', 'LineWidth', 1.3);
        ylabel('i_{fric}');
        yyaxis right;
        plot(pos_grid_um, i_avg, 'm-', 'LineWidth', 1.1);
        ylabel('i_{avg}');
        xlabel('位置 (um)');
        title(sprintf('i_{fric}, mean=%.3f', mean(i_fric)));
        grid on;

        sgtitle(sprintf('%s, V=%d mm/s, rep=%d', result.filename, V_target, rep_id));

        saveas(fig, fullfile(output_folder, sprintf('%s_result.png', name_only)));
    end
end

%% ========================================================================
%% 局部函数：按位置保留中间 keep_ratio 的候选点
%% ========================================================================

function idx_keep = keep_middle_position_points(candidate_idx, pos_vec, keep_ratio)

    pos_candidate = pos_vec(candidate_idx);

    pos_min = min(pos_candidate);
    pos_max = max(pos_candidate);
    pos_range = pos_max - pos_min;

    if pos_range <= eps
        idx_keep = candidate_idx;
        return;
    end

    pos_keep_start = pos_min + (1 - keep_ratio) / 2 * pos_range;
    pos_keep_end   = pos_max - (1 - keep_ratio) / 2 * pos_range;

    is_middle = pos_candidate >= pos_keep_start & pos_candidate <= pos_keep_end;

    idx_keep = candidate_idx(is_middle);
end

%% ========================================================================
%% 局部函数：位置排序并对重复位置取平均电流
%% ========================================================================

function [pos_unique, current_unique] = sort_and_average_duplicate_position(pos, current)

    pos = pos(:);
    current = current(:);

    valid = isfinite(pos) & isfinite(current);
    pos = pos(valid);
    current = current(valid);

    [pos_sorted, sort_idx] = sort(pos);
    current_sorted = current(sort_idx);

    [pos_unique, ~, group_id] = unique(pos_sorted);
    current_unique = accumarray(group_id, current_sorted, [], @mean);
end