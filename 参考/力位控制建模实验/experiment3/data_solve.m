%% 恒速上下行实验数据处理完整代码
% 作者：基于文档规范与实验需求编写
% 功能：处理单次往复运动实验数据，提取恒速段，计算摩擦电流
% 输入：包含下行和上行运动的实验数据文件
% 输出：恒速段数据、摩擦电流计算结果及可视化图形

clear; clc; close all;
fprintf('=== 恒速上下行实验数据处理开始 ===\n');

%% 1. 加载实验数据
% 请确保文件路径正确，并根据实际文件格式调整读取方式
filename = '50_1.csv'; % 您的数据文件名
fprintf('正在加载数据文件: %s\n', filename);

try
    % 读取CSV文件，假设第一行为表头
    data = readtable(filename);
    fprintf('数据加载成功，总数据点数: %d\n', height(data));
catch ME
    error('无法加载数据文件，请检查文件路径和格式。错误信息: %s', ME.message);
end

% 提取各数据列（请根据实际CSV文件的列名调整）
% 注意：您的数据列名可能略有不同，请检查实际表头
time = data{:, 1};          % 时间 (s)
velocity = data{:, 3};         % 实际速度 (mm/s)
current_cmd = data{:, 4};     % 电流命令 (可能是mA单位)
position = data{:, 5};     % 位置反馈 (mm)
force_fb = data{:, 7}; % 力反馈 (N)

% 检查数据基本统计
fprintf('数据统计:\n');
fprintf('  时间范围: %.3f 到 %.3f 秒\n', time(1), time(end));
fprintf('  位置范围: %.2f 到 %.2f mm\n', min(position), max(position));
fprintf('  速度范围: %.2f 到 %.2f mm/s\n', min(velocity), max(velocity));
fprintf('  电流范围: %.2f 到 %.2f\n', min(current_cmd), max(current_cmd));

%% 2. 数据可视化 - 原始信号
figure('Name', '原始实验数据', 'Position', [100, 100, 1000, 800]);

subplot(4,1,1);
plot(time, position, 'b-', 'LineWidth', 1.5);
ylabel('位置 (mm)');
title('位置随时间变化');
grid on;
xlim([time(1), time(end)]);

subplot(4,1,2);
plot(time, velocity, 'r-', 'LineWidth', 1.5);
ylabel('速度 (mm/s)');
title('速度随时间变化');
grid on;
xlim([time(1), time(end)]);
hold on;
% 标记零速度线
yline(0, 'k--', 'LineWidth', 0.5, 'Alpha', 0.5);

subplot(4,1,3);
plot(time, current_cmd, 'g-', 'LineWidth', 1.5);
ylabel('电流命令');
title('电流命令随时间变化');
grid on;
xlim([time(1), time(end)]);

subplot(4,1,4);
plot(time, force_fb, 'k-', 'LineWidth', 1.5);
ylabel('力反馈 (N)');
xlabel('时间 (s)');
title('力反馈随时间变化');
grid on;
xlim([time(1), time(end)]);
hold on;
% 标记零力线
yline(0, 'k--', 'LineWidth', 0.5, 'Alpha', 0.5);

%% 3. 分割下行与上行运动阶段
% 方法1: 通过位置信号找到运动反转点（位置最大值）
[~, idx_turn] = max(position);
fprintf('\n检测到运动反转点（位置最大值）在索引: %d (时间: %.3fs)\n', idx_turn, time(idx_turn));

% 方法2: 通过速度过零点找到反转点（备选方法）
% zero_crossings = find(diff(sign(velocity)) ~= 0);
% if ~isempty(zero_crossings)
%     idx_turn = zero_crossings(1); % 取第一个过零点
% end

% 分割数据
if idx_turn > 1 && idx_turn < length(time)
    % 下行阶段 (从开始到反转点)
    idx_down = 1:idx_turn;
    time_down = time(idx_down);
    vel_down = velocity(idx_down);
    current_down = current_cmd(idx_down);
    pos_down = position(idx_down);
    force_down = force_fb(idx_down);
    
    % 上行阶段 (从反转点到结束)
    idx_up = idx_turn:length(time);
    time_up = time(idx_up);
    vel_up = velocity(idx_up);
    current_up = current_cmd(idx_up);
    pos_up = position(idx_up);
    force_up = force_fb(idx_up);
    
    fprintf('运动分割结果:\n');
    fprintf('  下行阶段: %d 个数据点 (%.3f 到 %.3f 秒)\n', length(idx_down), time_down(1), time_down(end));
    fprintf('  上行阶段: %d 个数据点 (%.3f 到 %.3f 秒)\n', length(idx_up), time_up(1), time_up(end));
else
    error('运动反转点检测异常，请检查数据或手动指定分割点。');
end

% 可视化运动分割结果
figure('Name', '运动阶段分割', 'Position', [100, 100, 900, 400]);
subplot(1,2,1);
plot(time_down, pos_down, 'b.', 'MarkerSize', 6, 'DisplayName', '下行阶段');
hold on;
plot(time_up, pos_up, 'r.', 'MarkerSize', 6, 'DisplayName', '上行阶段');
xlabel('时间 (s)');
ylabel('位置 (mm)');
title('运动阶段分割 (基于位置)');
legend('Location', 'best');
grid on;

subplot(1,2,2);
plot(time_down, vel_down, 'b.', 'MarkerSize', 6, 'DisplayName', '下行速度');
hold on;
plot(time_up, vel_up, 'r.', 'MarkerSize', 6, 'DisplayName', '上行速度');
plot([time(idx_turn), time(idx_turn)], [min(velocity), max(velocity)], 'k--', 'LineWidth', 1, 'DisplayName', '反转点');
xlabel('时间 (s)');
ylabel('速度 (mm/s)');
title('速度阶段分割');
legend('Location', 'best');
grid on;

%% 4. 提取恒速运动段
% 设置实验参数
V_target = 50;        % 目标速度 (mm/s)，从文件名推测
V_tolerance = 6;      % 速度容差 (mm/s)
force_threshold = 20;  % 力阈值 (N)，用于排除异常接触
keep_ratio = 0.8;     % 保留中间位置百分比（剔除前后10%）

fprintf('\n恒速段提取参数:\n');
fprintf('  目标速度: %.1f mm/s\n', V_target);
fprintf('  速度容差: ±%.1f mm/s\n', V_tolerance);
fprintf('  力阈值: %.1f N\n', force_threshold);
fprintf('  保留位置区间: 中间 %.0f%%\n', keep_ratio*100);

% 提取下行恒速段
is_down_in_speed = (vel_down >= (V_target - V_tolerance)) & (vel_down <= (V_target + V_tolerance));
is_down_force_ok = abs(force_down) < force_threshold;
down_candidate_idx = find(is_down_in_speed & is_down_force_ok);

if ~isempty(down_candidate_idx)
    % 剔除加速/减速段（保留中间位置区间）
    pos_down_candidate = pos_down(down_candidate_idx);
    pos_down_range = [min(pos_down_candidate), max(pos_down_candidate)];
    pos_keep_start = pos_down_range(1) + (1-keep_ratio)/2 * diff(pos_down_range);
    pos_keep_end = pos_down_range(2) - (1-keep_ratio)/2 * diff(pos_down_range);
    
    is_down_in_middle = (pos_down_candidate >= pos_keep_start) & (pos_down_candidate <= pos_keep_end);
    idx_const_down = down_candidate_idx(is_down_in_middle);
    
    const_time_down = time_down(idx_const_down);
    const_vel_down = vel_down(idx_const_down);
    const_current_down = current_down(idx_const_down);
    const_pos_down = pos_down(idx_const_down);
else
    idx_const_down = [];
    const_time_down = [];
    const_vel_down = [];
    const_current_down = [];
    const_pos_down = [];
end

% 提取上行恒速段（注意上行目标速度为负）
is_up_in_speed = (vel_up <= (-V_target + V_tolerance)) & (vel_up >= (-V_target - V_tolerance));
is_up_force_ok = abs(force_up) < force_threshold;
up_candidate_idx = find(is_up_in_speed & is_up_force_ok);

if ~isempty(up_candidate_idx)
    pos_up_candidate = pos_up(up_candidate_idx);
    pos_up_range = [min(pos_up_candidate), max(pos_up_candidate)];
    pos_keep_start = pos_up_range(1) + (1-keep_ratio)/2 * diff(pos_up_range);
    pos_keep_end = pos_up_range(2) - (1-keep_ratio)/2 * diff(pos_up_range);
    
    is_up_in_middle = (pos_up_candidate >= pos_keep_start) & (pos_up_candidate <= pos_keep_end);
    idx_const_up = up_candidate_idx(is_up_in_middle);
    
    const_time_up = time_up(idx_const_up);
    const_vel_up = vel_up(idx_const_up);
    const_current_up = current_up(idx_const_up);
    const_pos_up = pos_up(idx_const_up);
else
    idx_const_up = [];
    const_time_up = [];
    const_vel_up = [];
    const_current_up = [];
    const_pos_up = [];
end

% 输出恒速段提取结果
fprintf('\n恒速段提取结果:\n');
if ~isempty(idx_const_down)
    fprintf('  下行恒速段: %d 个点，平均速度: %.2f mm/s\n', ...
        length(idx_const_down), mean(const_vel_down));
else
    fprintf('  下行恒速段: 未找到符合条件的点\n');
end

if ~isempty(idx_const_up)
    fprintf('  上行恒速段: %d 个点，平均速度: %.2f mm/s\n', ...
        length(idx_const_up), mean(const_vel_up));
else
    fprintf('  上行恒速段: 未找到符合条件的点\n');
end

% 检查是否有足够的恒速段数据
if isempty(idx_const_down) || isempty(idx_const_up)
    error('下行或上行恒速段数据不足，无法进行摩擦电流计算。请调整速度容差或力阈值。');
end

% 可视化恒速段提取结果
figure('Name', '恒速段提取结果', 'Position', [100, 100, 1000, 600]);

% 速度曲线与恒速段标记
subplot(2,2,1);
plot(time_down, vel_down, 'b-', 'LineWidth', 1, 'DisplayName', '下行速度');
hold on;
plot(time_up, vel_up, 'r-', 'LineWidth', 1, 'DisplayName', '上行速度');
plot(const_time_down, const_vel_down, 'bo', 'MarkerSize', 4, 'DisplayName', '下行恒速段');
plot(const_time_up, const_vel_up, 'ro', 'MarkerSize', 4, 'DisplayName', '上行恒速段');
yline(V_target, 'b--', 'Alpha', 0.5, 'DisplayName', sprintf('目标速度 %.0f', V_target));
yline(-V_target, 'r--', 'Alpha', 0.5, 'DisplayName', sprintf('目标速度 -%.0f', V_target));
xlabel('时间 (s)');
ylabel('速度 (mm/s)');
title('速度曲线与恒速段标记');
legend('Location', 'best');
grid on;

% 电流曲线与恒速段标记
subplot(2,2,2);
plot(time_down, current_down, 'b-', 'LineWidth', 1, 'DisplayName', '下行电流');
hold on;
plot(time_up, current_up, 'r-', 'LineWidth', 1, 'DisplayName', '上行电流');
plot(const_time_down, const_current_down, 'bo', 'MarkerSize', 4, 'DisplayName', '下行恒速段');
plot(const_time_up, const_current_up, 'ro', 'MarkerSize', 4, 'DisplayName', '上行恒速段');
xlabel('时间 (s)');
ylabel('电流命令');
title('电流曲线与恒速段标记');
legend('Location', 'best');
grid on;

% 位置-速度关系
subplot(2,2,3);
scatter(pos_down, vel_down, 10, 'b', 'filled', 'MarkerFaceAlpha', 0.3, 'DisplayName', '下行');
hold on;
scatter(pos_up, vel_up, 10, 'r', 'filled', 'MarkerFaceAlpha', 0.3, 'DisplayName', '上行');
scatter(const_pos_down, const_vel_down, 30, 'b', 'o', 'LineWidth', 1.5, 'DisplayName', '下行恒速段');
scatter(const_pos_up, const_vel_up, 30, 'r', 'o', 'LineWidth', 1.5, 'DisplayName', '上行恒速段');
xlabel('位置 (mm)');
ylabel('速度 (mm/s)');
title('位置-速度相图');
legend('Location', 'best');
grid on;

% 位置-电流关系
subplot(2,2,4);
scatter(pos_down, current_down, 10, 'b', 'filled', 'MarkerFaceAlpha', 0.3, 'DisplayName', '下行');
hold on;
scatter(pos_up, current_up, 10, 'r', 'filled', 'MarkerFaceAlpha', 0.3, 'DisplayName', '上行');
scatter(const_pos_down, const_current_down, 30, 'b', 'o', 'LineWidth', 1.5, 'DisplayName', '下行恒速段');
scatter(const_pos_up, const_current_up, 30, 'r', 'o', 'LineWidth', 1.5, 'DisplayName', '上行恒速段');
xlabel('位置 (mm)');
ylabel('电流命令');
title('位置-电流关系');
legend('Location', 'best');
grid on;

%% 5. 摩擦电流计算
fprintf('\n=== 摩擦电流计算 ===\n');

% 确定共同位置区间
common_pos_min = max(min(const_pos_down), min(const_pos_up));
common_pos_max = min(max(const_pos_down), max(const_pos_up));

if common_pos_min >= common_pos_max
    error('上下行恒速段位置区间无重叠，无法计算摩擦电流。');
end

fprintf('共同位置区间: [%.3f, %.3f] mm\n', common_pos_min, common_pos_max);

% 创建共同位置网格（精细插值）
pos_grid = linspace(common_pos_min, common_pos_max, 1000);

% 上下行电流插值到共同位置网格
% 使用线性插值，外推值设为NaN
current_down_interp = interp1(const_pos_down, const_current_down, pos_grid, 'linear', NaN);
current_up_interp = interp1(const_pos_up, const_current_up, pos_grid, 'linear', NaN);

% 移除插值产生的NaN值
valid_idx = ~isnan(current_down_interp) & ~isnan(current_up_interp);
pos_grid_valid = pos_grid(valid_idx);
current_down_valid = current_down_interp(valid_idx);
current_up_valid = current_up_interp(valid_idx);

if sum(valid_idx) < 10
    error('有效插值点过少，无法进行可靠计算。');
end

% 计算摩擦电流 i_fric = (i_down - i_up) / 2
i_fric = (current_down_valid - current_up_valid) / 2;

% 计算位置负载项 i_avg = (i_down + i_up) / 2 （用于验证）
i_avg = (current_down_valid + current_up_valid) / 2;

% 计算统计量
i_fric_mean = mean(i_fric);
i_fric_std = std(i_fric);
i_avg_mean = mean(i_avg);

fprintf('摩擦电流统计:\n');
fprintf('  平均值: %.4f\n', i_fric_mean);
fprintf('  标准差: %.4f\n', i_fric_std);
fprintf('  最小值: %.4f\n', min(i_fric));
fprintf('  最大值: %.4f\n', max(i_fric));
fprintf('  位置负载项平均值: %.4f\n', i_avg_mean);

% 摩擦电流随位置变化可视化
figure('Name', '摩擦电流计算结果', 'Position', [100, 100, 1200, 500]);

% 子图1: 上下行电流对比
subplot(1,3,1);
plot(pos_grid_valid, current_down_valid, 'b-', 'LineWidth', 1.5, 'DisplayName', '下行电流 i_{+}');
hold on;
plot(pos_grid_valid, current_up_valid, 'r-', 'LineWidth', 1.5, 'DisplayName', '上行电流 i_{-}');
xlabel('位置 (mm)');
ylabel('电流');
title('恒速段上下行电流对比');
legend('Location', 'best');
grid on;
xlim([common_pos_min, common_pos_max]);

% 子图2: 摩擦电流与位置负载项
subplot(1,3,2);
yyaxis left;
plot(pos_grid_valid, i_fric, 'k-', 'LineWidth', 1.5, 'DisplayName', '摩擦电流 i_{fric}');
ylabel('摩擦电流 i_{fric}');
hold on;
yline(i_fric_mean, 'k--', 'LineWidth', 1, 'DisplayName', sprintf('平均值=%.3f', i_fric_mean));

yyaxis right;
plot(pos_grid_valid, i_avg, 'm-', 'LineWidth', 1, 'DisplayName', '位置负载项 i_{avg}');
ylabel('位置负载项 i_{avg}');
yline(i_avg_mean, 'm--', 'LineWidth', 1, 'DisplayName', sprintf('平均值=%.3f', i_avg_mean));

xlabel('位置 (mm)');
title('摩擦电流与位置负载项');
legend('Location', 'best');
grid on;
xlim([common_pos_min, common_pos_max]);

% 子图3: 摩擦电流直方图
subplot(1,3,3);
histogram(i_fric, 20, 'FaceColor', [0.3, 0.3, 0.3], 'EdgeColor', 'k');
hold on;
xline(i_fric_mean, 'r-', 'LineWidth', 2, 'DisplayName', sprintf('均值=%.3f', i_fric_mean));
xline(i_fric_mean + i_fric_std, 'r--', 'LineWidth', 1, 'DisplayName', sprintf('±1σ=%.3f', i_fric_std));
xline(i_fric_mean - i_fric_std, 'r--', 'LineWidth', 1);
xlabel('摩擦电流');
ylabel('频数');
title('摩擦电流分布直方图');
legend('Location', 'best');
grid on;

%% 6. 结果保存与导出
fprintf('\n=== 结果保存 ===\n');

% 创建结果结构体
results = struct();
results.V_target = V_target;
results.V_tolerance = V_tolerance;
results.force_threshold = force_threshold;
results.keep_ratio = keep_ratio;

% 恒速段数据
results.const_down.time = const_time_down;
results.const_down.velocity = const_vel_down;
results.const_down.current = const_current_down;
results.const_down.position = const_pos_down;

results.const_up.time = const_time_up;
results.const_up.velocity = const_vel_up;
results.const_up.current = const_current_up;
results.const_up.position = const_pos_up;

% 摩擦电流结果
results.friction.pos_grid = pos_grid_valid;
results.friction.current_down = current_down_valid;
results.friction.current_up = current_up_valid;
results.friction.i_fric = i_fric;
results.friction.i_avg = i_avg;
results.friction.i_fric_mean = i_fric_mean;
results.friction.i_fric_std = i_fric_std;
results.friction.i_avg_mean = i_avg_mean;

% 保存结果到MAT文件
output_filename = sprintf('friction_results_V%d.mat', V_target);
save(output_filename, 'results');
fprintf('结果已保存到文件: %s\n', output_filename);

% 保存关键数据到CSV文件（可选）
csv_data = table(pos_grid_valid', current_down_valid', current_up_valid', i_fric', i_avg', ...
    'VariableNames', {'Position_mm', 'Current_Down', 'Current_Up', 'Friction_Current', 'Load_Current'});
csv_filename = sprintf('friction_data_V%d.csv', V_target);
writetable(csv_data, csv_filename);
fprintf('详细数据已保存到CSV文件: %s\n', csv_filename);

% 生成结果摘要报告
report_filename = sprintf('friction_report_V%d.txt', V_target);
fid = fopen(report_filename, 'w');
fprintf(fid, '恒速上下行实验数据处理报告\n');
fprintf(fid, '=============================\n\n');
fprintf(fid, '实验文件: %s\n', filename);
fprintf(fid, '处理时间: %s\n\n', datestr(now));
fprintf(fid, '实验参数:\n');
fprintf(fid, '  目标速度: %.1f mm/s\n', V_target);
fprintf(fid, '  速度容差: ±%.1f mm/s\n', V_tolerance);
fprintf(fid, '  力阈值: %.1f N\n', force_threshold);
fprintf(fid, '  位置保留比例: %.0f%%\n\n', keep_ratio*100);
fprintf(fid, '数据统计:\n');
fprintf(fid, '  总数据点数: %d\n', length(time));
fprintf(fid, '  下行阶段点数: %d\n', length(idx_down));
fprintf(fid, '  上行阶段点数: %d\n', length(idx_up));
fprintf(fid, '  下行恒速段点数: %d\n', length(idx_const_down));
fprintf(fid, '  上行恒速段点数: %d\n', length(idx_const_up));
fprintf(fid, '  共同位置区间: [%.3f, %.3f] mm\n\n', common_pos_min, common_pos_max);
fprintf(fid, '摩擦电流计算结果 (V=%.1f mm/s):\n', V_target);
fprintf(fid, '  平均值: %.4f\n', i_fric_mean);
fprintf(fid, '  标准差: %.4f\n', i_fric_std);
fprintf(fid, '  变异系数: %.2f%%\n', 100*i_fric_std/abs(i_fric_mean));
fprintf(fid, '  位置负载项平均值: %.4f\n\n', i_avg_mean);
fclose(fid);
fprintf('报告已保存到文件: %s\n', report_filename);

%% 7. 摩擦模型拟合（框架，需要多速度数据）
fprintf('\n=== 摩擦模型拟合说明 ===\n');
fprintf('要获得完整的摩擦模型 i_fric(V) = b0 + b1*V，需要在多个速度下重复实验。\n');
fprintf('当前速度 V=%.1f mm/s 下的摩擦电流平均值: %.4f\n', V_target, i_fric_mean);
fprintf('\n如需拟合模型，请进行以下操作:\n');
fprintf('1. 在不同速度下重复此实验（如 50, 80, 120, 180, 240 mm/s）\n');
fprintf('2. 对每个速度运行此脚本，记录 i_fric_mean 值\n');
fprintf('3. 使用以下代码框架进行线性拟合:\n');
fprintf('   V_list = [50, 80, 120, 180, 240]; %% 速度数组\n');
fprintf('   i_fric_list = [i50, i80, i120, i180, i240]; %% 对应摩擦电流\n');
fprintf('   p = polyfit(V_list, i_fric_list, 1);\n');
fprintf('   b0 = p(2); %% 库仑摩擦系数\n');
fprintf('   b1 = p(1); %% 黏性摩擦系数\n');
fprintf('   fprintf(''摩擦模型: i_fric = %.4f + %.4f * V\\n'', b0, b1);\n');

fprintf('\n=== 数据处理完成 ===\n');
fprintf('所有结果已保存，请查看生成的图形和文件。\n');