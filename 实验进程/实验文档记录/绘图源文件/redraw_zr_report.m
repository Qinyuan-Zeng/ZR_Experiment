function redraw_zr_report(out)
% Reproduce the ten report figures from the archived experiment tables and frames.
root = fileparts(fileparts(fileparts(fileparts(mfilename('fullpath')))));
s1 = fullfile(root,'分项实验','01_位置相关空载电流辨识');
s2 = fullfile(root,'分项实验','02_等效惯性电流辨识');
bc = @(name) read(fullfile(s1,'results','BC_position','Z-BC_20261124-192404_01.'+name+'.csv'));
d1 = @(name) read(fullfile(s1,'results','D','Z-D_20261124-203450_02','Z-D_20261124-203450_02.'+name+'.csv'));
b = @(name) read(fullfile(s2,'results','b','Z-BP_20261125-195157_221_35296_'+name+'.csv'));
d = @(name) read(fullfile(s2,'results','d','Z-DP_20261126-095126_343_29884_'+name+'.csv'));
k = 1.2;
blue = [0.05 .33 .58]; orange = [.79 .32 .13]; green = [.16 .49 .41];
speeds = ["2,000 脉冲/s","10,000 脉冲/s","40,000 脉冲/s"];
if ~isfolder(out), mkdir(out); end

A = read(fullfile(s1,'data','raw','Z-A1_20261124-144034_01.csv'));
f = canvas(320);
for pos = [8 16 24]
    z = A(A.target_mm==pos & A.stage=="hold",:);
    t = z.t_s-z.t_s(1);
    y = arrayfun(@(i) mean(z.current_fb_raw(t>=i & t<i+1)),0:29);
    plot(.5:29.5,k*y,'LineWidth',1.5);
end
labels('保持时间（s）','1 s 平均电流（mA）');
legend('8,000 脉冲','16,000 脉冲','24,000 脉冲','Location','eastoutside');
export(f,out,1);

T = bc("references");
f = canvas(315);
g = unique(T.group(T.phase=="B"),'stable');
for j=1:numel(g)
    z=sortrows(T(T.group==g(j),:),'position');
    plot(z.position*1000,k*z.ip,'LineWidth',1.45);
end
labels('位置（脉冲）','双向平均电流（mA）');
legend('第 1 轮','第 2 轮','Location','northwest');
export(f,out,2);

M=bc("B_nodes"); x=M.position_mm*1000; y=k*M.ip_raw;
q=linspace(x(1),x(end),1201); linear=polyval(polyfit(x,y,1),q);
curves=[linear;polyval(polyfit(x,y,2),q);interp1(x,y,q,'linear');interp1(x,y,q,'pchip')];
f=canvas(465); tiledlayout(2,1,'Padding','compact','TileSpacing','compact');
nexttile; plot(q,curves,'LineWidth',1.2);
plot(x(1:5:end),y(1:5:end),'.','Color',[.25 .25 .25]);
labels('位置（脉冲）','双向平均电流（mA）');
legend('全程线性','二次','分段线性','PCHIP','实测节点','Location','northwest','NumColumns',2);
nexttile; plot(q,curves-linear,'LineWidth',1.15);
labels('位置（脉冲）','相对线性预测的差值（mA）');
export(f,out,3);

f=canvas(325); plot(x,y,'Color',blue,'LineWidth',1.8);
for speed=[2 10 40]
    z=T(T.phase=="C" & T.speed==speed,:);
    groups=unique(z.group); positions=unique(z.position);
    for j=1:numel(groups), positions=intersect(positions,z.position(z.group==groups(j))); end
    avg=arrayfun(@(p) mean(z.ip(z.position==p)),positions);
    plot(positions*1000,k*avg,'LineWidth',1.25);
end
labels('位置（脉冲）','双向中心电流（mA）');
legend(["静态保持",speeds],'Location','northwest','NumColumns',2);
export(f,out,4);

M=bc("mode_comparison"); delta=k*[M.delta2,M.delta10,M.delta40];
f=canvas(330); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
nexttile; plot(M.position_mm*1000,delta,'LineWidth',1.25); yline(0,':','HandleVisibility','off');
labels('位置（脉冲）','恒速减静态（mA）'); legend(speeds,'Location','best','FontSize',8);
nexttile; bar([sqrt(mean(delta.^2));std(delta,1)]','grouped');
xticks(1:3); xticklabels({'2,000','10,000','40,000'});
labels('扫描速度（脉冲/s）','差值 RMSE（mA）'); ylim([0 11]); legend('原差值','去均值后','Location','north');
export(f,out,5);

P=d1("center_predictions"); W=d1("direction_metrics");
f=canvas(365); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
nexttile;
for i=1:2
    z=P(P.stage=="D"+i & P.window=="second_4",:);
    scatter(z.position_pulse,k*z.error_B_raw,17,'filled','MarkerFaceAlpha',.58);
end
yline(0,':','HandleVisibility','off');
labels('位置（脉冲）','双向中心预测误差（mA）');
legend('未见位置','另时段复测','Location','best','FontSize',9);
nexttile;
for i=1:2
    for direction=[1 -1]
        z=W(W.stage=="D"+i & W.model=="B" & W.scope=="all" & W.direction==direction,:);
        z=sortrows(z,'window'); style='-o'; if i==2, style='--s'; end
        color=blue; if direction<0, color=orange; end
        plot(1:height(z),k*z.mean_error_raw,style,'Color',color,'LineWidth',1.2);
    end
end
yline(0,':','HandleVisibility','off'); xticks(1:4);
labels('保持窗口（s）','单方向平均残差（mA）');
legend('未见位置 正向','未见位置 反向','复测 正向','复测 反向','Location','eastoutside','FontSize',8);
export(f,out,6);

F=read(fullfile(s2,'results','a1','Z-A1_20261125-112511_825_34600_fits.csv'));
z=F(F.session_id=="Z-A1_20261125-111250_763_46824" & F.pair_id==1 & F.window_ms==21,:);
f=canvas(330); tiledlayout(1,2,'Padding','compact','TileSpacing','compact'); a=nexttile; h=nexttile;
for role=["accel","decel"]
    r=z(z.role==role,:); raw=read(fullfile(s2,'data','raw',r.run_id+'.csv'));
    meta=fileread(fullfile(s2,'manifest',r.run_id+'.txt'));
    tick=str2double(regexp(meta,'core_period_ns=(\d+)','tokens','once'));
    t=mod(raw.card_tick_raw-raw.card_tick_raw(1),2^32)*tick/1e6-r.crossing_since_first_frame_ms;
    use=abs(t)<=20; plot(a,t(use),raw.encoder_position_raw(use)-15976,'LineWidth',1.35);
    use=abs(t)<=6; plot(h,t(use),k*raw.current_feedback_raw(use),'LineWidth',1.15);
end
xline(a,0,':','HandleVisibility','off'); xline(h,[-2.5 2.5],':','HandleVisibility','off');
for ax=[a h], xlabel(ax,'相对过点时间（ms）'); end
ylabel(a,'相对评价位置的位移（脉冲）'); ylabel(h,'反馈电流（mA）');
legend(a,'加速轨迹','减速轨迹','Location','northwest'); legend(h,'加速轨迹','减速轨迹','Location','northwest');
export(f,out,7);

P=b("pairs"); P=P(P.valid==1,:); V=b("acceleration_validation");
f=canvas(350); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
for j=1:2
    nexttile; v=[16000 20000]; z=P(P.nominal_v_pulse_s==v(j),:);
    acc=unique(z.nominal_A_pulse_s2); colors=[blue;orange];
    scatter(z.delta_a_pulse_s2/1e6,k*z.delta_I_corr_raw,24,colors(j,:),'filled','MarkerFaceAlpha',.32);
    mx=arrayfun(@(n) mean(z.delta_a_pulse_s2(z.nominal_A_pulse_s2==n)),acc)/1e6;
    my=arrayfun(@(n) mean(z.delta_I_corr_raw(z.nominal_A_pulse_s2==n)),acc)*k;
    plot(mx,my,'-o','Color',colors(j,:),'LineWidth',1.6,'MarkerFaceColor','w');
    q=[.56 1.32]; plot(q,q*1e6*3.0306147541113107e-5*k,':','Color',[.3 .3 .3],'LineWidth',1.3);
    xlim(q); ylim([0 60]); titles=["16,000 脉冲/s","20,000 脉冲/s"]; title(titles(j));
    labels('实测加速度差（10^6 脉冲/s²）','修正电流差（mA）');
    legend('逐对数据','各档均值','中段模型','Location','northwest','FontSize',8);
end
export(f,out,8);

z=V(V.method=="leave_tier_out" & V.training_scope=="pooled",:);
f=canvas(340); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
for j=1:2
    nexttile;
    for v=[16000 20000]
        q=sortrows(z(z.speed_pulse_s==v,:),'held_out_acc_pulse_s2');
        y=q.bias_raw; if j==2, y=q.rmse_raw; end
        plot(q.held_out_acc_pulse_s2/1000,k*y,'-o','LineWidth',1.4);
    end
    if j==1
        yline([-4.8 4.8],'--','HandleVisibility','off'); yline(0,':','HandleVisibility','off');
        labels('留出加速度档（千脉冲/s²）','平均偏差（mA）');
    else
        yline(7.2,'--','HandleVisibility','off'); ylim([0 12]);
        labels('留出加速度档（千脉冲/s²）','预测 RMSE（mA）');
    end
    xticks(300:100:650); legend('16,000 脉冲/s','20,000 脉冲/s','Location','best','FontSize',8);
end
export(f,out,9);

P=d("pairs"); P=sortrows(P(P.valid==1,:),'planned_pair_id');
f=canvas(350); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
nexttile; p=1:height(P); actual=k*P.delta_I_corr_raw; predicted=k*P.predicted_raw;
for j=p, plot([j j],[actual(j) predicted(j)],'-','Color',[.74 .77 .80],'HandleVisibility','off'); end
plot(p,actual,'o','Color',blue,'MarkerFaceColor',blue);
plot(p,predicted,'d','Color',orange,'MarkerFaceColor',orange);
xlim([.5 5.5]); xticks(p); ylim([26 38]); labels('独立运动对','修正电流差（mA）');
legend('实测','冻结模型预测','Location','southwest','FontSize',9);
nexttile; e=actual-predicted; val=[abs(mean(e)),sqrt(mean(e.^2))]; limit=[4.8 7.2];
bar([val;limit]'); ylim([0 8.4]); xticks([1 2]); xticklabels({'绝对平均偏差','RMSE'});
for j=1:2
    text(j-.14,val(j)+.15,sprintf('%.2f',val(j)),'HorizontalAlignment','center','FontSize',9);
    text(j+.14,limit(j)+.15,sprintf('%.2f',limit(j)),'HorizontalAlignment','center','FontSize',9);
end
ylabel('验收指标（mA）'); legend('实测指标','协议上限','Location','northwest','FontSize',9);
export(f,out,10);
end

function t=read(p)
t=readtable(p,'TextType','string');
end

function f=canvas(h)
f=figure('Visible','off','Color','white','Position',[100 100 920 h],...
    'DefaultAxesFontName','SimSun','DefaultAxesFontSize',11,'DefaultTextFontName','SimSun',...
    'DefaultAxesBox','on','DefaultAxesTickDir','in','DefaultAxesXGrid','on',...
    'DefaultAxesYGrid','on','DefaultAxesGridAlpha',.14,'DefaultAxesNextPlot','add',...
    'DefaultLegendBox','off','DefaultAxesColorOrder',[.05 .33 .58;.79 .32 .13;.16 .49 .41;.45 .35 .59]);
end

function labels(x,y)
xlabel(x); ylabel(y);
end

function export(f,out,n)
name=string(fullfile(out,sprintf('图%02d',n)));
savefig(f,name+'.fig');
exportgraphics(f,name+'.png','Resolution',400);
exportgraphics(f,name+'.svg','ContentType','vector');
close(f);
end
