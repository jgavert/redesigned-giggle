using System.IO;
using Sharpmake;

// Both the library and the executable can share these base settings, so create
// a base class for both projects.
namespace CoroutineStealer
{
    abstract class CommonProject : Project
    {
        public string ProjectBasePath = @"[project.SharpmakeCsPath]/../";
        public CommonProject()
        {
            AddTargets(new Target(
                Platform.win64,
                DevEnv.vs2019,
                Optimization.Debug | Optimization.Release,
                OutputType.Lib));
            IsFileNameToLower = false;
        }
        
        public virtual void ConfigureAll(Configuration conf, Target target)
        {
            conf.Name = @"[target.Optimization] [target.OutputType]";
            conf.ProjectFileName = "[project.Name]_[target.DevEnv]_[target.Platform]_[target.OutputType]";
            conf.TargetLibraryPath = "[project.ProjectBasePath]/output/[conf.ProjectFileName]";
            conf.ProjectPath = Path.Combine("[project.SharpmakeCsPath]/projects/[project.Name]");
            conf.IncludePaths.Add(SourceRootPath);
            conf.Options.Add(Options.Vc.General.WindowsTargetPlatformVersion.Latest);
            conf.Options.Add(Options.Vc.General.WarningLevel.Level0);
            conf.Options.Add(Options.Vc.General.WarningLevel.Level1);
            conf.Options.Add(Options.Vc.General.WarningLevel.Level2);
            conf.Options.Add(Options.Vc.General.WarningLevel.Level3);
            conf.Options.Add(Options.Vc.Librarian.TreatLibWarningAsErrors.Enable);
            conf.Options.Add(Options.Vc.General.TreatWarningsAsErrors.Enable);
            conf.Options.Add(Options.Vc.Compiler.ConformanceMode.Enable);
            conf.Options.Add(Options.Vc.Compiler.CppLanguageStandard.Latest);
            conf.Options.Add(Options.Vc.Compiler.EnhancedInstructionSet.AdvancedVectorExtensions2);
            conf.Defines.Add("WIN32_LEAN_AND_MEAN");
            conf.Defines.Add("NOMINMAX");
            conf.AdditionalCompilerOptions.Add(new string[] { "/await", "/EHsc" });
            conf.Options.Add(Options.Vc.Linker.GenerateDebugInformation.Enable);
            conf.Options.Add(Options.Vc.General.DebugInformation.ProgramDatabase);
            //conf.AdditionalCompilerOptions.Add(new string[] { "/wd4530" });
            if (target.Optimization == Optimization.Debug) {
                configureDebug(conf, target);
            } else
            {
                configureRelease(conf, target);
            }
        }
        public virtual void configureDebug(Configuration conf, Target target)
        {
            conf.Options.Add(Options.Vc.Compiler.Optimization.Disable);
            //conf.Options.Add(Options.Vc.Linker.);
        }
        public virtual void configureRelease(Configuration conf, Target target)
        {
            conf.Options.Add(Options.Vc.Compiler.Optimization.FullOptimization);
        }
    }
}