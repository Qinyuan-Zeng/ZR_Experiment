% Rebuild from the completed B session. Currents are in drive feedback raw units.
root = fileparts(fileparts(mfilename('fullpath')));
session = 'Z-B_20261124-153415_01';
T = readtable(fullfile(root,'data','raw',[session '.csv']),'TextType','string');
H = T(T.stage=="hold" & T.target_mm>0 & T.target_mm<32,:);
x = (2:2:30)';
paired = zeros(numel(x),5);
for i=1:numel(x)
    for g=1:5
        for d=[-1 1]
            S = H(H.target_mm==x(i) & H.group==g & H.direction==d,:);
            t = S.t_s;
            y = S.current_fb_raw;
            start = t(end)-1;
            keep = t>start;
            value = trapz([start;t(keep)],[interp1(t,y,start);y(keep)]);
            paired(i,g) = paired(i,g)+value/2;
        end
    end
end
y = mean(paired,2);
s = std(paired,0,2);
p = polyfit(x,y,1);
r = y-polyval(p,x);
blue = [0 .447 .741]; red = [.85 .325 .098];
fig = figure('Visible','off','Units','inches','Position',[1 1 8 6],'Color','w');
ax = axes(fig,'Position',[.13 .43 .83 .47]); hold(ax,'on');
h1 = errorbar(x,y,s,'o','Color',blue,'MarkerFaceColor','w','LineWidth',1.3,'CapSize',5);
h2 = plot(x,polyval(p,x),'--','Color',red,'LineWidth',1.4);
title('\fontname{SimSun}空载保持电流的位置特性与线性拟合','FontWeight','normal');
ylabel('\fontname{SimSun}保持电流\fontname{Times New Roman} (raw)');
legend([h1 h2],{'\fontname{SimSun}双向平均 ± 组间标准差','\fontname{SimSun}线性拟合'},'Location','northwest','Box','off');
formula = ['$\hat{I}_p(x) = ' sprintf('%.2f + %.2fx',p(2),p(1)) '$'];
text(.97,.12,formula,'Interpreter','latex','Units','normalized','HorizontalAlignment','right','FontSize',13);
ax2 = axes(fig,'Position',[.13 .15 .83 .19]); hold(ax2,'on');
yline(0,'-','Color',[.3 .3 .3]); errorbar(x,r,s,'o','Color',blue,'MarkerFaceColor',blue,'LineWidth',1.1,'CapSize',5);
ylabel('\fontname{SimSun}拟合残差\fontname{Times New Roman} (raw)'); xlabel('\fontname{SimSun}位置\fontname{Times New Roman} (mm)');
ylim([-40 40]); yticks(-40:20:40);
for a=[ax ax2]
    set(a,'Box','on','TickDir','in','FontName','Times New Roman','FontSize',12,'LineWidth',.75,'GridColor',[.89 .89 .89],'GridAlpha',1);
    grid(a,'on'); xlim(a,[1 31]); xticks(a,2:2:30);
end
out = fullfile(root,'data','figures','B',[session '.线性候选基线']);
if ~isfolder(fileparts(out)), mkdir(fileparts(out)); end
savefig(fig,[out '.fig']); exportgraphics(fig,[out '.svg'],'ContentType','vector'); exportgraphics(fig,[out '.png'],'Resolution',400);
fprintf('Slope %.6f, intercept %.6f, RMSE %.6f raw\n',p(1),p(2),sqrt(mean(r.^2)));
