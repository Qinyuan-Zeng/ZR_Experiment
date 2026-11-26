function plot_a1_results
% Rebuild the combined tables with analyze_a1.mjs (later session first).
root = fileparts(fileparts(mfilename('fullpath')));
id = "Z-A1_20261125-112511_825_34600";
prefix = fullfile(root,"results","a1",id);
out = fullfile(root,"data","figures","A1",id);
if ~isfolder(out), mkdir(out); end
p = readtable(prefix+"_pairs.csv",TextType="string");
p = sortrows(p(p.valid==1,:),{'session_id','pair_id'});
f = readtable(prefix+"_fits.csv",TextType="string");
w = readtable(prefix+"_current_windows.csv",TextType="string");
s = jsondecode(fileread(prefix+"_summary.json"));
assert(height(p)==s.repeat.repeatPairs);
c = [0.04 0.29 0.52; 0.78 0.29 0.10];
st = {'-','--'};
roles = ["accel","decel"];
leg = {'加速轨迹','减速轨迹'};
% First chronological formal pair. Align each record on its encoder crossing.
fig = canvas(); t = tiledlayout(fig,1,2,'TileSpacing','compact','Padding','compact');
ax = [nexttile(t),nexttile(t)];
for j=1:2
    row = f(f.session_id==p.session_id(1) & f.pair_id==p.pair_id(1) & ...
        f.window_ms==21 & f.role==roles(j),:);
    raw = readtable(fullfile(root,"data","raw",row.run_id+".csv"));
    meta = fileread(fullfile(root,"manifest",row.run_id+".txt"));
    tick = str2double(regexp(meta,'core_period_ns=(\d+)','tokens','once'));
    assert(isfinite(tick));
    ms = (raw.card_tick_raw-raw.card_tick_raw(1))*tick/1e6-row.crossing_since_first_frame_ms;
    use = abs(ms)<=20;
    plot(ax(1),ms(use),raw.encoder_position_raw(use)-15976,st{j},Color=c(j,:),LineWidth=1.7);
    use = abs(ms)<=6;
    plot(ax(2),ms(use),raw.current_feedback_raw(use),st{j},Color=c(j,:),LineWidth=1.3);
end
for a=ax
    xlabel(a,mix('相对过点时间','ms'));
    legend(a,leg,'FontName','SimSun','Location','northwest','Box','off');
end
xline(ax(1),0,':',HandleVisibility='off'); xlim(ax(1),[-20 20]);
ylabel(ax(1),'相对评价位置的位移（脉冲）',FontName='SimSun');
title(ax(1),'首个正式对在 15976 脉冲处对齐',FontName='SimSun');
xline(ax(2),[-2.5 2.5],':',HandleVisibility='off'); xlim(ax(2),[-6 6]);
ylabel(ax(2),mix('反馈电流','raw'));
title(ax(2),'原始电流与中心 5 ms 提取窗',FontName='SimSun');
saveplots(fig,out,id+"_01_alignment");

fig=canvas(); fig.Position(4)=700;
t=tiledlayout(fig,2,2,'TileSpacing','compact','Padding','compact');
n=height(p); k=1:n;
a=nexttile(t); plot(a,k,abs(p.delta_v_mm_s)*1000,'o',Color=c(1,:),MarkerFaceColor=c(1,:));
yline(a,s.condition.speedTol*1000,'--','500',Color=c(2,:));
ylim(a,[0 580]); ylabel(a,'速度差绝对值（脉冲/s）',FontName='SimSun');
title(a,'速度匹配与预设容差（21 ms）',FontName='SimSun'); pairAxis(a,n);
a=nexttile(t);
for j=1:3
    widths=[5 9 15]; part=sortrows(w(w.window_ms==widths(j),:),{'session_id','pair_id'});
    style={'-o','--s',':^'}; colors=[c;0.27 0.45 0.29];
    plot(a,k,part.delta_corr,style{j},Color=colors(j,:),LineWidth=1.35);
end
ylabel(a,mix('修正电流差','raw')); title(a,'电流提取窗口敏感性',FontName='SimSun');
legend(a,{'5 ms','9 ms','15 ms'},Location='southwest',Box='off',NumColumns=3);
ylim(a,[18 23.5]); pairAxis(a,n);
a=nexttile(t);
for j=1:2
    v=zeros(n,1);
    for i=1:n
        selected=f.session_id==p.session_id(i) & f.pair_id==p.pair_id(i) & f.window_ms==21 & f.role==roles(j);
        v(i)=f.a_mm_s2(selected)*1000;
    end
    plot(a,k,v,[st{j} 'o'],Color=c(j,:),LineWidth=1.35);
end
ylim(a,[-550000 550000]); ylabel(a,'加速度（脉冲/s^{2}）',FontName='SimSun');
legend(a,leg,'FontName','SimSun','Location','east','Box','off');
title(a,'匹配点实际加速度（21 ms）',FontName='SimSun'); pairAxis(a,n);
a=nexttile(t); plot(a,k,p.alpha_raw_per_mm_s2/1000,'o',Color=c(1,:),MarkerFaceColor=c(1,:));
errorbar(a,n+1,s.repeat.meanAlpha/1000,(s.repeat.meanAlpha-s.repeat.ci95(1))/1000,...
    's',Color=c(2,:),LineWidth=1.6,MarkerFaceColor=c(2,:));
ylabel(a,'\alpha  [raw/(\fontname{SimSun}脉冲\fontname{Times New Roman}/s^{2})]');
title(a,'逐对系数与均值的 95% 置信区间',FontName='SimSun');
xticks(a,1:n+1); xticklabels(a,[string(k),"均值"]); a.FontName='SimSun';
xlim(a,[0.5 n+1.5]); xlabel(a,'按采集时序的有效对',FontName='SimSun');
saveplots(fig,out,id+"_02_repeatability");
end

function f=canvas()
f=figure('Visible','off','Color','w','Position',[100 100 1100 385],...
    'DefaultAxesFontName','Times New Roman','DefaultAxesFontSize',13,...
    'DefaultAxesBox','on','DefaultAxesTickDir','in','DefaultAxesXGrid','on',...
    'DefaultAxesYGrid','on','DefaultAxesGridAlpha',0.12,'DefaultAxesNextPlot','add');
end
function label=mix(name,unit)
label=['\fontname{SimSun}' name '（\fontname{Times New Roman}' unit '\fontname{SimSun}）'];
end
function pairAxis(a,n)
xticks(a,1:n); xlim(a,[0.5 n+0.5]); xlabel(a,'按采集时序的有效对',FontName='SimSun');
end
function saveplots(f,out,name)
savefig(f,fullfile(out,name+".fig"));
exportgraphics(f,fullfile(out,name+".svg"),ContentType='vector');
exportgraphics(f,fullfile(out,name+".png"),Resolution=400); close(f);
end
