% Inputs: references.csv contains the two dense B runs and C development groups only.
root=fileparts(fileparts(mfilename('fullpath')));
out=fullfile(root,'results','BC_position');
figures=fullfile(root,'data','figures','BC_position');
if ~isfolder(figures), mkdir(figures); end
analysis='Z-BC_20261124-192404_01';
tablePath=@(name)fullfile(out,[analysis '.' name]);
figureStem=fullfile(figures,analysis);
T=readtable(tablePath('references.csv'),'TextType','string');
D=cell(1,4); Y=D; X=D; P=D; N=D;
labels=["静态保持","2 mm/s","10 mm/s","40 mm/s"];
names=["常值","线性","二次","分段线性","PCHIP"];
speed=[0 2 10 40]; scores=[]; models=struct;
for j=1:4
    if j==1, t=T(T.phase=="B",:); else, t=T(T.phase=="C"&T.speed==speed(j),:); end
    groups=unique(t.group); x=unique(t.position);
    for g=groups', x=intersect(x,t.position(t.group==g)); end
    for k=1:numel(groups)
        z=sortrows(t(t.group==groups(k)&ismember(t.position,x),:),'position');
        Y{j}(:,k)=z.ip; P{j}(:,k)=z.positive; N{j}(:,k)=z.negative;
    end
    X{j}=x; y=Y{j}; models(j).x=x; models(j).ip=mean(y,2);
    for m=1:5
        err=[];
        for k=1:size(y,2)
            tr=mean(y(:,(1:size(y,2))~=k),2);
            for i=2:numel(x)-1
                keep=(1:numel(x))~=i;
                err(end+1)=prediction(x(keep),tr(keep),x(i),m)-y(i,k);
            end
        end
        scores(end+1,:)=[speed(j),m,numel(err),sqrt(mean(err.^2)),prctile(abs(err),95)];
    end
end
disp('speed model n RMSE P95');disp(scores);
writetable(array2table(scores,'VariableNames',{'speed_0_is_B','model_1to5','n','RMSE_raw','P95_raw'}),tablePath('model_scores.csv'));
save(tablePath('candidate_nodes.mat'),'models','speed','labels','names','scores');
figure1(); plot(X{1},Y{1},'LineWidth',1.1);legend('第 1 轮','第 2 轮','Location','northwest'); axeslabels('位置 (mm)','双向平均电流 (raw)','静态位置电流的重复性');finish(figureStem,1);
disp('B run differences: rms, mean run2-run1, max abs'); e=diff(Y{1},1,2);disp([sqrt(mean(e.^2)),mean(e),max(abs(e))]);
figure1(); tiledlayout(1,2,'TileSpacing','compact');for j=1:2
    nexttile;plot(X{j},[mean(P{j},2),mean(N{j},2),mean(Y{j},2)],'LineWidth',1.2);
    legend('正向','反向','双向平均','Location','northwest');axeslabels('位置 (mm)','电流 (raw)',labels(j));
end;finish(figureStem,2);
figure1(); hold on;for j=1:4,plot(X{j},mean(Y{j},2),'LineWidth',1.2);end
legend(labels,'Location','northwest');axeslabels('位置 (mm)','双向平均电流 (raw)','静态保持与恒速扫描');finish(figureStem,3);
figure1();x=X{1};y=mean(Y{1},2);plot(x,y,'.','MarkerSize',7);hold on;
for m=1:5,plot(x,prediction(x,y,x,m),'LineWidth',1.1);end
legend(["实测",names],'Location','northwest');axeslabels('位置 (mm)','电流 (raw)','静态候选位置模型');finish(figureStem,4);
figure1();tiledlayout(1,2,'TileSpacing','compact');for j=4:5
    nexttile;bar(reshape(scores(:,j),5,4));set(gca,'XTick',1:5,'XTickLabel',names);set(gca,'YScale','log');
    legend(labels,'Location','northeast');tt=["均方根误差","绝对误差第 95 百分位"];axeslabels('候选模型','误差 (raw，对数轴)',tt(j-3));
end;sgtitle('开发组交叉验证','FontName','SimSun');finish(figureStem,5);
xx=X{4};for j=2:3,xx=intersect(xx,X{j});end
b=interp1(X{1},mean(Y{1},2),xx,'pchip');delta=[]; dyn=[];
for j=2:4
    [~,ix]=ismember(xx,X{j});dyn(:,j-1)=mean(Y{j}(ix,:),2);delta(:,j-1)=dyn(:,j-1)-b;
end
disp('common range; mean C-B; rms C-B; centered rms; RMS speed differences 2-10,10-40,2-40');disp(xx([1 end])');disp(mean(delta));disp(sqrt(mean(delta.^2)));disp(std(delta,1));disp(sqrt(mean([dyn(:,1)-dyn(:,2),dyn(:,2)-dyn(:,3),dyn(:,1)-dyn(:,3)].^2)));
writetable(array2table([xx,b,dyn,delta],'VariableNames',{'position_mm','B','C2','C10','C40','delta2','delta10','delta40'}),tablePath('mode_comparison.csv'));
figure1();tiledlayout(2,1,'TileSpacing','compact');for j=1:2,nexttile;d=delta;if j==2,d=d-mean(d);end;plot(xx,d,'LineWidth',1);yline(0,':');legend(labels(2:4),'Location','best');tt=["恒速减静态","扣除各速度平均偏差后"];axeslabels('位置 (mm)','差值 (raw)',tt(j));end;finish(figureStem,6);
A=readtable(fullfile(root,'data','raw','Z-A1_20261124-144034_01.csv'),'TextType','string');figure1();hold on;
for pos=[8 16 24],z=A(A.target_mm==pos&A.stage=="hold",:);t=z.t_s-z.t_s(1);v=arrayfun(@(i)mean(z.current_fb_raw(t>=i&t<i+1)),0:29);plot(.5:29.5,v,'LineWidth',1.3);end
legend('8 mm','16 mm','24 mm');axeslabels('连续保持时间 (s)','1 s 均值 (raw)','A1 短时保持电流');finish(figureStem,7);
A=readtable(fullfile(root,'results','Z-A2_20261124-150201_03.repeatability.csv'));figure1();hold on;
for d=[1 -1],z=A(A.direction==d,:);errorbar(z.position+d*.12,z.mean_raw,z.repeat_std_raw,'o','LineWidth',1.4);end
xticks([8 16 24]);xlim([6 26]);legend('正向','反向','Location','northwest');axeslabels('目标位置 (mm)','均值 ± 标准差 (raw)','A2 独立重复到位');finish(figureStem,8);
function v=prediction(x,y,q,m)
if m==5,v=interp1(x,y,q,'pchip');elseif m==4,v=interp1(x,y,q,'linear');else,v=polyval(polyfit(x,y,m-1),q);end
end
function figure1()
figure('Visible','off','Color','w','Position',[50 50 1000 550],'DefaultAxesFontName','Times New Roman','DefaultLegendFontName','SimSun');
end
function axeslabels(x,y,t)
grid on;box on;set(gca,'TickDir','in','GridAlpha',.15,'FontSize',12);
xlabel(x,'FontName','SimSun');ylabel(y,'FontName','SimSun');title(t,'FontName','SimSun','FontWeight','normal');
end
function finish(figureStem,n)
names={'B_repeatability','B_direction_means','BC_position_curves','B_candidate_models', ...
    'development_cv','C_minus_B','A1_short_hold','A2_repeatability'};
stem=sprintf('%s.%02d-%s',figureStem,n,names{n});
exportgraphics(gcf,[stem '.png'],'Resolution',320);exportgraphics(gcf,[stem '.svg'],'ContentType','vector');close(gcf);
end

