import java.io.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

class FileUtil {
    public static List<String> getFilePathByBFS(String dictionaryPath){
        List<String> ans = new ArrayList<>();
        searchFilePathByBFS(dictionaryPath, file -> {
            if(!file.isDirectory()){
                ans.add(file.getAbsolutePath());
            }
            return false;
        });
        return ans;
    }

    public interface FileFoundCallBack{
        boolean onFound(File file);
    }

    public static void searchFilePathByBFS(String dictionaryPath, FileFoundCallBack callBack){
        Queue<String> queue = new ArrayDeque<>();
        queue.add(dictionaryPath);
        boolean breakFlag = false;
        while (!breakFlag && !queue.isEmpty()){
            String currentDicPath = queue.poll();
            File currentDic = new File(currentDicPath);
            if(!currentDic.exists() || !currentDic.isDirectory() ){
                continue;
            }
            File[] files = currentDic.listFiles();
            assert files != null;
            for(File file: files){
                if(callBack.onFound(file)){
                    breakFlag = true;
                    break;
                }
                if(file.isDirectory()){
                    queue.add(file.getAbsolutePath());
                }
            }
        }
    }
}


public class CommandGenerator {
    
    public static void main(String args[]) throws Exception {
		
		var sourcePath = args[0];
		var targetPath = args[1];
		
        var isCppProject = new AtomicBoolean(false);
        StringBuilder fileList = new StringBuilder();
        StringBuilder linkCmd = new StringBuilder();
        Set<String> linkedDic = new HashSet<>();
        FileUtil.searchFilePathByBFS(sourcePath, file -> {
            if(file.isDirectory()){
                return false;
            }
            String path = file.getAbsolutePath();
            String suffix = path.substring(path.lastIndexOf(".") + 1);
            switch (suffix){
                case "hpp":
                case "hh":
                case "H":
                case "hxx":
                    isCppProject.set(true);
                case "h":
                    String linkPath = file.getParent();
                    if(!linkedDic.contains(linkPath)){
                        linkCmd.append(" ").append("-I ").append(linkPath);
                        linkedDic.add(linkPath);
                    }
                    break;
                case "cpp":
                case "CPP":
                case "c++":
                case "cxx":
                case "C":
                case "cc":
                case "cp":
                    isCppProject.set(true);
                case "c":
                    fileList.append(" ").append(path);
                    break;
                default:
                        break;
            }
            return false;
        });
        String compileCmd = String.format("%s %s %s -o %s","clang++ -std=c++17 -O2 -lm",
            fileList, linkCmd, targetPath);
		System.out.println(compileCmd);
    }
}


