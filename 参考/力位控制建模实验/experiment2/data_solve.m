%% 实验二：往复步进数据 - 位置负载电流辨识 (不依赖速度信号版)
% 数据文件: 往复1.csv
% 目标: 从往复步进数据中提取各位置点的稳态保持电流。
% 方法: 基于位置信号的稳定性检测静止段。

clear; close all; clc;

%% 1. 加载数据
filename = '往复5.csv';
data = readtable(filename);
% 请根据实际文件表头调整列号
t = data{:, 1};          % 时间 (s)
% vel = data{:, 3};       % 速度信号不再使用
i_cmd = data{:, 4};      % 电流指令
pos = data{:, 8};        % 编码器位置 (um)
dt = mean(diff(t));      % 平均采样间隔
fs = 1/dt;               % 采样频率

fprintf('数据加载完成。时长: %.2f 秒, 采样数: %d, 采样率: %.1f Hz\n', t(end), length(t), fs);

%% 2. 参数设置 (基于位置信号)
window_time = 0.1;               % 计算位置标准差的滑动窗口时长 (s)
window_len = round(window_time * fs); % 窗口长度 (点数)
pos_std_win_threshold = 3.0;     % 窗口内位置标准差阈值 (um)，低于此值认为可能静止
min_still_duration = 0.8;        % 最小静止段持续时间 (s)
stable_duration = 1.0;           % 用于取平均的稳态段持续时间 (s)，取静止段最后这部分
pos_std_seg_threshold = 2.0;     % 整个静止段的位置标准差阈值 (um)，用于最终校验
step_size_expected = 1000;       % 期望的步进距离 (um)，用于方向判断和验证

%% 3. 计算滑动窗口位置标准差 (用于初步检测静止)
fprintf('正在计算滑动窗口位置标准差...\n');
pos_std_win = zeros(size(pos));
half_win = floor(window_len / 2);
for i = 1:length(pos)
    idx_start = max(1, i - half_win);
    idx_end = min(length(pos), i + half_win); % 取索引 0逐渐涨到窗口大小再回到0
    pos_std_win(i) = std(pos(idx_start:idx_end)); % 每个索引位置得到的窗口再计算其位置标准差
end

% 初步标记"潜在静止点"：窗口内位置波动很小
is_potential_still = pos_std_win < pos_std_win_threshold; % 向量和数的比较

%% 4. 聚类并筛选有效的静止段
transitions = diff([0; is_potential_still; 0]); 
run_starts = find(transitions == 1); % 找到聚类的静止段开始的索引
run_ends = find(transitions == -1) - 1;

still_segments = []; % 格式: [start_idx, end_idx, duration, avg_pos]
valid_seg_counter = 0;
for k = 1:length(run_starts)
    seg_start = run_starts(k);
    seg_end = run_ends(k);
    seg_duration = t(seg_end) - t(seg_start);
    
    % 条件1: 持续时间需足够长
    if seg_duration < min_still_duration
        continue;
    end
    
    % 条件2: 整个段的位置需高度稳定 (最终校验)
    pos_seg = pos(seg_start:seg_end);
    pos_std_seg = std(pos_seg);
    if pos_std_seg > pos_std_seg_threshold
        continue;
    end
    
    % 这是一个有效的静止段
    valid_seg_counter = valid_seg_counter + 1;
    avg_pos_seg = mean(pos_seg);
    still_segments(valid_seg_counter, :) = [seg_start, seg_end, seg_duration, avg_pos_seg];
end
num_segments = valid_seg_counter;
fprintf('检测到 %d 个有效静止段。\n', num_segments); % 15000那里连成一整块了，算作了一个有效静止段

%% 5. 提取每个静止段的稳态数据并判断方向
hold_positions = zeros(num_segments, 1);
hold_currents = zeros(num_segments, 1);
hold_directions = zeros(num_segments, 1); % 1:下行, -1:上行

for seg = 1:num_segments
    seg_start = still_segments(seg, 1);
    seg_end = still_segments(seg, 2);
    
    % 确定用于分析的稳态数据区间 (每段的最后 stable_duration 秒)
    analysis_end_time = t(seg_end);
    analysis_start_time = max(t(seg_start), analysis_end_time - stable_duration); % 取最后1s来分析数据
    analysis_start_idx = find(t >= analysis_start_time, 1, 'first'); % 时间与索引的转换，找到最后1s开始点的索引位置
    analysis_end_idx = seg_end;
    
    if isempty(analysis_start_idx) || (analysis_end_idx - analysis_start_idx < 2)
        fprintf('  段 %d: 稳态分析区间过短，跳过。\n', seg);
        continue;
    end
    
    % 提取区间内的位置和电流
    pos_seg_stable = pos(analysis_start_idx:analysis_end_idx);
    i_cmd_seg_stable = i_cmd(analysis_start_idx:analysis_end_idx);
    
    % 计算该稳态段的平均位置和电流
    avg_pos = mean(pos_seg_stable);
    avg_current = mean(i_cmd_seg_stable);
    hold_positions(seg) = avg_pos;
    hold_currents(seg) = avg_current;
    
    % 判断运动方向 (方法A: 利用前一个静止段的位置)
    if seg == 1
        % 第一个段，无法用前一段判断，采用方法B: 观察段前一段时间的趋势
        pre_samples = round(0.2 / dt); % 看静止开始前0.2秒
        idx_before = max(1, seg_start - pre_samples):seg_start;
        if isempty(idx_before) || length(idx_before)<2
            direction = 0;
        else
            pos_diff_init = pos(seg_start) - pos(idx_before(1));
            if pos_diff_init < 0
                direction = 1; % 下行
            elseif pos_diff_init > 0
                direction = -1; % 上行
            else
                direction = 0;
            end
        end
    else
        % 利用前一个静止段的位置判断
        prev_avg_pos = still_segments(seg-1, 4);
        if avg_pos < prev_avg_pos
            direction = 1; % 当前位置 < 前一个位置，说明是下行过来的
        elseif avg_pos > prev_avg_pos
            direction = -1; % 上行
        else
            direction = 0; % 位置相同，异常
        end
    end
    hold_directions(seg) = direction;
    
    fprintf('  段 %2d: 位置=%8.1f, 电流=%7.2f, 方向=%2d\n', ...
            seg, avg_pos, avg_current, direction);
end

%% 6. 区分上下行数据
idx_down = hold_directions == 1;
idx_up = hold_directions == -1;
% 忽略方向未知的点(idx=0)

pos_down = hold_positions(idx_down);
i_down = hold_currents(idx_down);
pos_up = hold_positions(idx_up);
i_up = hold_currents(idx_up);

fprintf('\n下行点: %d 个, 上行点: %d 个。\n', sum(idx_down), sum(idx_up));

%% 7. 计算负载电流与滞回电流 (与之前相同)
if isempty(pos_down) || isempty(pos_up)
    error('未能提取到足够的上行或下行数据，请检查参数或数据。');
end

% 排序
[pos_down_sorted, sort_idx_down] = sort(pos_down);
i_down_sorted = i_down(sort_idx_down);
[pos_up_sorted, sort_idx_up] = sort(pos_up);
i_up_sorted = i_up(sort_idx_up);

% 创建共同位置网格并插值
all_pos = unique([pos_down_sorted; pos_up_sorted]);
common_pos_grid = linspace(min(all_pos), max(all_pos), 500)';
i_down_interp = interp1(pos_down_sorted, i_down_sorted, common_pos_grid, 'linear', 'extrap');
i_up_interp = interp1(pos_up_sorted, i_up_sorted, common_pos_grid, 'linear', 'extrap');

% 计算负载与滞回
i_load = (i_down_interp + i_up_interp) / 2;
i_hys = (i_down_interp - i_up_interp) / 2;

%% 8. 拟合负载曲线
valid_idx = common_pos_grid >= min(all_pos) & common_pos_grid <= max(all_pos);
common_pos_valid = common_pos_grid(valid_idx);
i_load_valid = i_load(valid_idx);

P = polyfit(common_pos_valid, i_load_valid, 1); % 线性拟合
i_load_fit = polyval(P, common_pos_valid);

y_mean = mean(i_load_valid);
SS_tot = sum((i_load_valid - y_mean).^2);
SS_res = sum((i_load_valid - i_load_fit).^2);
R2 = 1 - (SS_res / SS_tot);
fprintf('\n负载电流线性拟合: i_load = %.6f * x + %.4f, R² = %.4f\n', P(1), P(2), R2);

%% 9. 绘图展示结果
figure('Position', [100, 100, 1200, 900]);

% 子图1: 原始位置信号与滑动窗口标准差
subplot(4,2,1);
yyaxis left;
plot(t, pos, 'b-', 'LineWidth', 0.5);
ylabel('位置 (um)');
yyaxis right;
plot(t, pos_std_win, 'r-', 'LineWidth', 0.5);
ylabel('窗口位置标准差 (um)');
xlabel('时间 (s)');
title('原始位置信号与滑动窗口标准差');
grid on;
legend('位置', '位置标准差(窗口)', 'Location', 'best');

% 子图2: 原始电流信号，标注静止段
subplot(4,2,2);
plot(t, i_cmd, 'k-', 'LineWidth', 0.5);
ylabel('电流 Icmd');
xlabel('时间 (s)');
title('原始电流信号与检测到的静止段');
grid on; hold on;
% 高亮检测到的静止段
for seg = 1:num_segments
    seg_start = still_segments(seg, 1);
    seg_end = still_segments(seg, 2);
    area(t([seg_start, seg_end]), [max(ylim), max(ylim)], ...
         'FaceColor', [0.9 0.7 0.7], 'EdgeColor', 'none', 'FaceAlpha', 0.3, 'ShowBaseLine','off');
end
% 标记稳态点
scatter(t(still_segments(:,2)), hold_currents, 40, 'g', 'filled');
hold off;
legend('电流', '检测的静止段', '稳态点', 'Location', 'best');

% 子图3: 上下行保持电流散点
subplot(4,2,3);
scatter(pos_down, i_down, 80, 'b^', 'filled', 'DisplayName', '下行保持电流');
hold on;
scatter(pos_up, i_up, 80, 'rv', 'filled', 'DisplayName', '上行保持电流');
xlabel('位置 (um)'); ylabel('保持电流');
title('上下行稳态保持电流散点图');
grid on; legend('Location', 'best');

% 子图4: 负载与滞回电流
subplot(4,2,4);
yyaxis left;
plot(common_pos_grid, i_load, 'g-', 'LineWidth', 2, 'DisplayName', 'i_{load}(计算)');
hold on;
plot(common_pos_valid, i_load_fit, 'm--', 'LineWidth', 2, 'DisplayName', sprintf('拟合 (R^2=%.3f)', R2));
ylabel('i_{load} (负载电流)');
yyaxis right;
plot(common_pos_grid, i_hys, 'c-', 'LineWidth', 1.5, 'DisplayName', 'i_{hys} (滞回电流)');
ylabel('i_{hys} (滞回电流)');
xlabel('位置 (um)'); title('负载电流与滞回电流');
grid on; legend('Location', 'best');

% 子图5: 负载电流拟合残差
subplot(4,2,5);
residuals = i_load_valid - i_load_fit;
plot(common_pos_valid, residuals, 'ko', 'MarkerSize', 4);
xlabel('位置 (um)'); ylabel('残差');
title('负载电流线性拟合残差');
grid on; yline(0, 'r--');

% 子图6: 滞回电流 vs 位置
subplot(4,2,6);
plot(common_pos_grid, i_hys, 'b-', 'LineWidth', 1.5);
xlabel('位置 (um)'); ylabel('i_{hys}');
title('滞回电流 vs 位置');
grid on;
avg_hys = mean(i_hys(valid_idx));
yline(avg_hys, 'r--', sprintf('均值: %.3f', avg_hys));
legend('滞回电流', sprintf('均值=%.3f', avg_hys), 'Location', 'best');

% 子图7: 静止段持续时间分布
subplot(4,2,7);
histogram(still_segments(:,3), 20, 'FaceColor', [0.2 0.6 0.8]);
xlabel('静止段持续时间 (s)');
ylabel('频次');
title('静止段持续时间分布');
grid on;

% 子图8: 位置序列 (检查步进规律)
subplot(4,2,8);
plot(1:num_segments, still_segments(:,4), 'bo-', 'LineWidth', 1, 'MarkerFaceColor', 'b');
xlabel('静止段序号');
ylabel('平均位置 (um)');
title('静止段位置序列');
grid on;

sgtitle('实验二：基于位置稳定性的负载电流辨识 (不依赖速度信号)', 'FontSize', 14, 'FontWeight', 'bold');

%% 10. 输出关键结果
fprintf('\n===== 处理结果摘要 =====\n');
fprintf('负载电流模型 (线性): i_load(x) = %.6f * x + %.4f\n', P(1), P(2));
fprintf('滞回电流平均值: %.4f (范围: [%.4f, %.4f])\n', ...
        avg_hys, min(i_hys(valid_idx)), max(i_hys(valid_idx)));
fprintf('静止段位置序列: \n');
disp(still_segments(:,4)');