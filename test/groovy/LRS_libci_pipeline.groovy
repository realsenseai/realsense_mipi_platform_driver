import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }

	options {
		timestamps()
			timeout(time: 30, unit: 'MINUTES')
	}

	parameters {
		booleanParam(
			name: 'REBOOT',
			description: 'When set the artifacts will be installed to target agent and the agent rebooted',
			defaultValue: false,
			)
		string(
			name: 'ARTIFACTS',
			description: 'Jenkins job for artifacts to be installed on target agent',
			defaultValue: "D4xx_Kernel_Module_Jetson_JP6"
			)
		string(
			name: 'BUILD',
			description: 'Build number for artifacts. Leave empty for last successful'
			)
	}

	stages {
		stage('Get artifacts') {
			when {
				expression { params.REBOOT == true }
			}
			steps {
				script {
					def buildSelector
						if (params.BUILD?.trim()) {
							buildSelector = specific(params.BUILD)
						} else {
							buildSelector = lastSuccessful()
						}

					copyArtifacts filter: '**/*.tar.bz2',
						      projectName: params.ARTIFACTS,
						      flatten: true,
						      selector: buildSelector,
						      target: 'artifacts/'
				}
			}
		}
		stage('Install artifacts') {
			when {
				expression { params.REBOOT == true }
			}
			steps {
				sh """#!/bin/sh
					rm -rf lib/modules
					tar -xf artifacts/rootfs.tar.bz2
					# external script on agent to install artifacts
					sudo install.tegra.artifacts.sh
				"""
			}
			post {
				success {
					sh 'nohup sudo reboot &'
				}
			}
		}
		stage('Pytest') {
			steps {
				sh """#!/bin/sh
					pytest --tb=no -s test
				"""
			}
		}
	}
}
