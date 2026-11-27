function redraw_zr_report_comparisons()
% Revised report figures 6, 7, 11 and 12. Reads archived model scores,
% frozen position nodes, D validation predictions/metrics, B leave-tier-out
% scores and D matched-pair summary. Feedback conversion is 1.2 mA/count.
% Outputs FIG, SVG and 400 dpi PNG next to this script's report.
here=fileparts(mfilename('fullpath'));
root=fileparts(fileparts(fileparts(here)));
out=fullfile(fileparts(here),'ZR实验阶段报告','comparisons');
if ~isfolder(out), mkdir(out); end
pos=fullfile(root,'分项实验','01_位置相关空载电流辨识');
inertia=fullfile(root,'分项实验','02_等效惯性电流辨识','results');
scale=1.2;
blue=[0 .447 .741]; orange=[.851 .325 .098];
dark=[.23 .23 .23]; green=[.14 .47 .39]; violet=[.49 .34 .59];

cv=read(fullfile(pos,'results','BC_position','Z-BC_20261124-192404_01.model_scores.csv'));
speed=[0 2 10 40]; groups={'B 静态','C 2 mm/s','C 10 mm/s','C 40 mm/s'};
metric={cv.RMSE_raw,cv.P95_raw}; modelColors=[blue;green;orange;violet];
f=canvas(435);
tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
for j=1:2
    nexttile; values=zeros(4,5);
    for i=1:4
        rows=cv.speed_0_is_B==speed(i);
        ids=cv.model_1to5(rows); values(i,ids)=scale*metric{j}(rows).';
    end
    bars=barh(1:4,values(:,2:5),.8);
    paint(bars,modelColors);
    groupAxis(groups); xlim([0 ceil(max(values(:,2:5),[],'all')/5)*5]);
    titles={'(a) 均方根误差','(b) 绝对误差 P95'};
    label(titles{j});
    xlabel('电流误差（mA）','FontName','SimSun');
    top=text(.025,.965,sprintf('常值模型（mA）   %.1f / %.1f / %.1f / %.1f',values(:,1)), ...
        'Units','normalized','VerticalAlignment','top','FontName','SimSun','FontSize',10);
    ylim([.03 4.55]);
    if j==1, key=legend(bars,{'线性','二次','分段线性','PCHIP'},'FontName','SimSun','Box','off','NumColumns',4); end
end
key.Layout.Tile='south';
savePlot(f,out,6);

id="Z-D_20261124-203450_02";
d=@(suffix) read(fullfile(pos,'results','D',id,id+"."+suffix+".csv"));
P=d("center_predictions"); M=d("full_metrics");
N=sortrows(read(fullfile(pos,'models','position_B_primary_v1.nodes.csv')),'position_pulse');
P=P(isfinite(P.B_raw) & isfinite(P.current_raw),:);
lower=100*floor(scale*min([N.current_raw;P.current_raw])/100);
upper=100*ceil(scale*max([N.current_raw;P.current_raw])/100);
f=canvas(620); tiledlayout(2,2,'Padding','compact','TileSpacing','compact');
for j=1:3
    if j==3, nexttile([1 2]); else, nexttile; end
    phase="D"+j; allRows=P(P.stage==phase,:); speeds=unique(allRows.speed_pulse_s);
    h=plot(N.position_pulse/1000,scale*N.current_raw,'-','Color',dark,'LineWidth',1.2);
    for i=1:numel(speeds)
        z=allRows(allRows.speed_pulse_s==speeds(i),:);
        m=M(M.stage==phase & M.scope=="all" & M.speed_pulse_s==speeds(i),:);
        assert(height(m)==1 && m.n==height(z));
        assert(abs(sqrt(mean(z.error_B_raw.^2))-m.rmse_raw)<1e-9);
        [g,x]=findgroups(z.position_pulse/1000);
        y=scale*splitapply(@mean,z.current_raw,g);
        if j<3
            lo=scale*splitapply(@min,z.current_raw,g);
            hi=scale*splitapply(@max,z.current_raw,g);
            h(i+1)=errorbar(x,y,y-lo,hi-y,'o','MarkerSize',3.5,'CapSize',3, ...
                'Color',blue,'MarkerFaceColor','w','LineWidth',.85);
            info(i)=string(sprintf('n = %d  /  RMSE %.2f  /  P95 %.2f mA',m.n,scale*m.rmse_raw,scale*m.p95_abs_raw));
        else
            colors=[blue;orange;green]; lines={'--','-.',':'};
            h(i+1)=plot(x,y,lines{i},'Color',colors(i,:),'LineWidth',1.5);
            info(i)=string(sprintf('%g mm/s   n = %d  /  RMSE %.2f  /  P95 %.2f mA', ...
                speeds(i)/1000,m.n,scale*m.rmse_raw,scale*m.p95_abs_raw));
        end
    end
    xlabel('位置（mm）','FontName','SimSun'); ylabel('双向中心电流（mA）','FontName','SimSun');
    xlim([1 31]); ylim([lower upper]);
    titles={'(a) D1 未见位置','(b) D2 同日另时段','(c) D3 恒速留出'};
    label(titles{j});
    if j<3, items=["冻结模型", "实测均值及范围   "+info(1)];
    else, items=["冻结模型",info(1:3)]; end
    legend(h,items,'Location','northwest','Box','off','FontName','SimSun','FontSize',9.3);
    clear h info
end
savePlot(f,out,7);

V=read(fullfile(inertia,'b','Z-BP_20261125-195157_221_35296_acceleration_validation.csv'));
V=V(V.method=="leave_tier_out" & V.training_scope=="pooled",:);
tiers=unique(V.held_out_acc_pulse_s2);
values=zeros(numel(tiers),2,2);
for j=1:2
    speed=[16000 20000];
    z=sortrows(V(V.speed_pulse_s==speed(j),:),'held_out_acc_pulse_s2');
    assert(isequal(z.held_out_acc_pulse_s2,tiers));
    values(:,j,1)=scale*z.bias_raw;
    values(:,j,2)=scale*z.rmse_raw;
end
f=canvas(430); tiledlayout(1,2,'Padding','compact','TileSpacing','compact');
for j=1:2
    nexttile; h=barh(1:numel(tiers),values(:,:,j),.78);
    paint(h,[blue;orange]); groupAxis(string(tiers/1000));
    ylabel('留出加速度档（千脉冲/s²）','FontName','SimSun');
    if j==1
        xline(0,':','Color',dark); xline([-4.8 4.8],'--','Color',dark);
        xlim([-12.5 12.5]); label('(a) 平均预测偏差');
        xlabel('实测减预测（mA）','FontName','SimSun');
        key=legend(h,{'16,000 脉冲/s','20,000 脉冲/s'},'Box','off','NumColumns',2,'FontName','SimSun');
    else
        xline(7.2,'--','Color',dark); xlim([0 12.5]); label('(b) 预测 RMSE');
        xlabel('均方根误差（mA）','FontName','SimSun');
    end
end
key.Layout.Tile='south';
savePlot(f,out,11);

prefix=fullfile(inertia,'d','Z-DP_20261126-095126_343_29884');
P=read(prefix+"_pairs.csv"); P=sortrows(P(P.valid==1,:),'planned_pair_id');
S=jsondecode(fileread(prefix+"_summary.json"));
actual=scale*P.delta_I_corr_raw; prediction=scale*P.predicted_raw; residual=actual-prediction;
assert(height(P)==5 && max(abs(residual-scale*P.residual_raw))<1e-9);
assert(abs(sqrt(mean(residual.^2))-scale*S.rmse)<1e-9);
assert(abs(sqrt(mean(actual.^2))-scale*S.baselineRmse)<1e-9);
f=canvas(460); tiledlayout(2,2,'Padding','compact','TileSpacing','compact');
nexttile(1); pairs=1:5;
h=bar(pairs,[actual,prediction],.78); paint(h,[blue;orange]);
xlim([.5 5.5]); ylim([0 43]); xticks(pairs);
ylabel('修正电流差（mA）','FontName','SimSun'); label('(a) 独立运动对');
legend(h,{'实测','冻结模型预测'},'Location','northoutside','Orientation','horizontal','Box','off','FontName','SimSun');
nexttile(3); h=bar(pairs,residual,.54); paint(h,blue);
yline(0,':','Color',dark); xlim([.5 5.5]); ylim([-8 0.65]); xticks(pairs);
for i=pairs, text(i,residual(i)-.34,sprintf('%.2f',residual(i)), ...
    'HorizontalAlignment','center','VerticalAlignment','top','FontSize',10); end
xlabel('独立运动对','FontName','SimSun'); ylabel('残差（mA）','FontName','SimSun');
label('(b) 实测减预测');
nexttile(2,[2 1]); value=scale*[S.rmse S.baselineRmse abs(S.bias)];
limits=scale*[S.limits.rmse S.limits.absBias];
y=[1 2 3.1]; color=[blue;orange;blue];
for i=1:3
    plot([0 value(i)],[y(i) y(i)],'-','Color',color(i,:),'LineWidth',2);
    plot(value(i),y(i),'o','MarkerFaceColor',color(i,:),'Color',color(i,:),'MarkerSize',6);
    text(value(i)+.65,y(i),sprintf('%.2f',value(i)),'FontSize',11);
end
plot(limits(1)*[1 1],[.57 2.42],'--','Color',dark);
plot(limits(2)*[1 1],[2.79 3.5],'--','Color',dark);
text(limits(1)+.55,.56,sprintf('RMSE 上限 %.2f',limits(1)),'FontName','SimSun','FontSize',10);
text(limits(2)+.55,3.52,sprintf('绝对偏差上限 %.2f',limits(2)),'FontName','SimSun','FontSize',10);
set(gca,'YDir','reverse','YTick',y,'YTickLabel',{'冻结模型 RMSE','零惯性基线 RMSE','绝对平均偏差'},'YGrid','off');
xlim([0 36]); ylim([.32 3.78]); label('(c) 模型对比与协议验收');
xlabel('评价指标（mA）','FontName','SimSun');
savePlot(f,out,12);
end

function t=read(path)
t=readtable(path,'TextType','string');
end

function f=canvas(h)
f=figure('Visible','off','Color','white','Position',[80 60 990 h], ...
    'DefaultAxesColor','white','DefaultAxesFontName','Times New Roman', ...
    'DefaultAxesFontSize',11,'DefaultTextFontName','Times New Roman', ...
    'DefaultAxesBox','on','DefaultAxesTickDir','in','DefaultAxesLineWidth',.75, ...
    'DefaultAxesXGrid','on','DefaultAxesYGrid','on','DefaultAxesGridAlpha',.13, ...
    'DefaultAxesNextPlot','add');
end

function label(text)
title(text,'FontName','SimSun','FontWeight','normal','FontSize',12);
end

function groupAxis(items)
set(gca,'YDir','reverse','YTick',1:numel(items),'YTickLabel',items,'YGrid','off');
end

function paint(bars,colors)
for j=1:numel(bars)
    bars(j).FaceColor=colors(j,:);
    bars(j).EdgeColor=.74*colors(j,:);
    bars(j).LineWidth=.6;
end
end

function savePlot(f,out,n)
stem=fullfile(out,sprintf('图%02d',n));
savefig(f,[stem '.fig']);
exportgraphics(f,[stem '.svg'],'ContentType','vector');
exportgraphics(f,[stem '.png'],'Resolution',400);
close(f)
end
