package cn.sdrstudio.app;
final class MapProjection {
    static double wrap(double degrees){return ((degrees+180)%360+360)%360-180;}
    static double x(double lon){return (lon+180)/360;}
    static double y(double lat){lat=Math.max(-85.05112878,Math.min(85.05112878,lat));double a=Math.toRadians(lat);return (1-Math.log(Math.tan(a)+1/Math.cos(a))/Math.PI)/2;}
    static double latitude(double y){return Math.toDegrees(Math.atan(Math.sinh(Math.PI*(1-2*y))));}
    static double deltaX(double lon,double center){double d=x(lon)-x(center);return d-Math.floor(d+.5);}
    static double metersPerPixel(double lat,int zoom){return 40075016.686*Math.cos(Math.toRadians(lat))/(256*(1<<zoom));}
}
